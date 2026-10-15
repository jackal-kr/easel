#include "core/history.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* One undo step. Normally it is just the rectangle that changed, with the
 * pixels from before and after. A "full" step keeps the whole picture instead,
 * because its size changed (resize, rotate, ...) and a rectangle can't say
 * that. Redo is simply "put the after-pixels back". */
typedef struct
{
  EaselRect r;
  uint32_t *before; /* r.width * r.height pixels (full step: bw * bh) */
  uint32_t *after;  /* same, (full step: aw * ah) */
  bool      full;
  int       bw, bh, aw, ah; /* only used by full steps */
  bool      spilled;        /* pixels live in the spill file, before and after back to back */
  off_t     off;
} Step;

/* A hole in the spill file that a later step can reuse. */
typedef struct
{
  off_t off, len;
} Extent;

struct _EaselHistory
{
  Step  *steps;
  size_t count;    /* steps stored */
  size_t cap;      /* allocated slots in `steps` */
  size_t pos;      /* steps[0..pos) are applied, steps[pos..count) are what redo would bring back */
  size_t bytes;     /* pixels stored in all steps, in memory and on disk */
  size_t max_bytes;
  long   saved;    /* value of pos at the last save; -1 when that state is gone for good */

  /* Spilling: when the steps held in memory exceed `ram_limit`, the oldest of
   * them move to a temporary file that is already unlinked, so it vanishes
   * when we exit or crash. Undo and redo read a step back when they need it. */
  size_t  ram_bytes;   /* part of `bytes` that is in memory */
  size_t  ram_limit;   /* SIZE_MAX: never spill */
  char   *spill_dir;
  int     fd;          /* -1 until the first spill */
  off_t   file_end;
  Extent *holes;
  size_t  n_holes, cap_holes;
};

static size_t
step_nb (const Step *s)
{
  return (s->full ? (size_t) s->bw * s->bh : (size_t) s->r.width * s->r.height) * sizeof (uint32_t);
}

static size_t
step_na (const Step *s)
{
  return (s->full ? (size_t) s->aw * s->ah : (size_t) s->r.width * s->r.height) * sizeof (uint32_t);
}

static size_t
step_bytes (const Step *s)
{
  return step_nb (s) + step_na (s);
}

/* ---- The spill file: a first-fit allocator over byte ranges ---- */

static off_t
file_alloc (EaselHistory *h, off_t len)
{
  for (size_t i = 0; i < h->n_holes; i++)
    if (h->holes[i].len >= len)
      {
        off_t off = h->holes[i].off;
        h->holes[i].off += len;
        h->holes[i].len -= len;
        if (h->holes[i].len == 0)
          memmove (h->holes + i, h->holes + i + 1, (h->n_holes - i - 1) * sizeof *h->holes), h->n_holes--;
        return off;
      }
  off_t off = h->file_end;
  h->file_end += len;
  return off;
}

static void
file_release (EaselHistory *h, off_t off, off_t len)
{
  size_t i = 0;

  while (i < h->n_holes && h->holes[i].off < off)
    i++;
  if (h->n_holes == h->cap_holes)
    {
      size_t cap = h->cap_holes ? h->cap_holes * 2 : 16;
      Extent *grown = realloc (h->holes, cap * sizeof *grown);
      if (grown == NULL)
        return; /* the space is lost until the history is cleared; harmless */
      h->holes = grown;
      h->cap_holes = cap;
    }
  memmove (h->holes + i + 1, h->holes + i, (h->n_holes - i) * sizeof *h->holes);
  h->holes[i] = (Extent) { off, len };
  h->n_holes++;
  /* Merge with the next and the previous hole. */
  if (i + 1 < h->n_holes && h->holes[i].off + h->holes[i].len == h->holes[i + 1].off)
    {
      h->holes[i].len += h->holes[i + 1].len;
      memmove (h->holes + i + 1, h->holes + i + 2, (h->n_holes - i - 2) * sizeof *h->holes);
      h->n_holes--;
    }
  if (i > 0 && h->holes[i - 1].off + h->holes[i - 1].len == h->holes[i].off)
    {
      h->holes[i - 1].len += h->holes[i].len;
      memmove (h->holes + i, h->holes + i + 1, (h->n_holes - i - 1) * sizeof *h->holes);
      h->n_holes--;
      i--;
    }
  /* A hole at the very end gives its disk space back. */
  if (h->holes[i].off + h->holes[i].len == h->file_end)
    {
      h->file_end = h->holes[i].off;
      h->n_holes--;
      if (ftruncate (h->fd, h->file_end) != 0)
        { /* only a missed chance to shrink */ }
    }
}

static bool
io_all (int fd, void *buf, size_t n, off_t off, bool write_it)
{
  char *p = buf;

  while (n > 0)
    {
      ssize_t r = write_it ? pwrite (fd, p, n, off) : pread (fd, p, n, off);
      if (r <= 0)
        return false;
      p += r;
      off += r;
      n -= (size_t) r;
    }
  return true;
}

static bool
open_spill (EaselHistory *h)
{
  if (h->fd >= 0)
    return true;

  const char *dir = h->spill_dir != NULL ? h->spill_dir : getenv ("TMPDIR");
  if (dir == NULL || dir[0] == '\0')
    dir = "/tmp";
  char *path = malloc (strlen (dir) + 32);
  if (path == NULL)
    return false;
  sprintf (path, "%s/easel-undo-XXXXXX", dir);
  int fd = mkstemp (path);
  if (fd >= 0)
    {
      unlink (path); /* gone from the file system now, freed by the kernel on exit */
      fcntl (fd, F_SETFD, FD_CLOEXEC);
    }
  free (path);
  h->fd = fd;
  return fd >= 0;
}

/* Moves one step's pixels to the file. On any failure the step simply stays
 * in memory. */
static bool
spill_step (EaselHistory *h, Step *s)
{
  size_t nb = step_nb (s), na = step_na (s);

  if (!open_spill (h))
    return false;
  off_t off = file_alloc (h, (off_t) (nb + na));
  if (!io_all (h->fd, s->before, nb, off, true) || !io_all (h->fd, s->after, na, off + (off_t) nb, true))
    {
      file_release (h, off, (off_t) (nb + na));
      return false;
    }
  free (s->before);
  free (s->after);
  s->before = s->after = NULL;
  s->spilled = true;
  s->off = off;
  h->ram_bytes -= nb + na;
  return true;
}

static void
spill_oldest (EaselHistory *h)
{
  /* The newest step always stays in memory, so Undo right after drawing is instant. */
  for (size_t i = 0; h->ram_bytes > h->ram_limit && i + 1 < h->count; i++)
    if (!h->steps[i].spilled && !spill_step (h, &h->steps[i]))
      return; /* disk trouble: keep the rest in memory */
}

/* The pixels of one side of a step. Returns NULL if reading the file failed.
 * `*owned` is set to a buffer the caller must free (spilled steps). */
static const uint32_t *
step_pixels (EaselHistory *h, const Step *s, bool after, uint32_t **owned)
{
  *owned = NULL;
  if (!s->spilled)
    return after ? s->after : s->before;

  size_t n = after ? step_na (s) : step_nb (s);
  uint32_t *buf = malloc (n);
  if (buf == NULL)
    return NULL;
  if (!io_all (h->fd, buf, n, s->off + (after ? (off_t) step_nb (s) : 0), false))
    {
      free (buf);
      return NULL;
    }
  *owned = buf;
  return buf;
}

static void
free_step (EaselHistory *h, Step *s)
{
  h->bytes -= step_bytes (s);
  if (s->spilled)
    file_release (h, s->off, (off_t) step_bytes (s));
  else
    h->ram_bytes -= step_bytes (s);
  free (s->before);
  free (s->after);
}

EaselHistory *
easel_history_new (size_t max_bytes)
{
  EaselHistory *h = calloc (1, sizeof *h);
  if (h != NULL)
    {
      h->max_bytes = max_bytes;
      h->ram_limit = SIZE_MAX;
      h->fd = -1;
    }
  return h;
}

void
easel_history_set_spill (EaselHistory *h, const char *dir, size_t ram_limit)
{
  free (h->spill_dir);
  h->spill_dir = dir != NULL ? strdup (dir) : NULL;
  h->ram_limit = ram_limit;
  spill_oldest (h);
}

void
easel_history_clear (EaselHistory *h)
{
  for (size_t i = 0; i < h->count; i++)
    free_step (h, &h->steps[i]);
  h->count = h->pos = 0;
  h->saved = 0;
}

void
easel_history_free (EaselHistory *h)
{
  if (h == NULL)
    return;
  easel_history_clear (h);
  if (h->fd >= 0)
    close (h->fd);
  free (h->holes);
  free (h->spill_dir);
  free (h->steps);
  free (h);
}

static bool
append_step (EaselHistory *h, Step *step)
{
  Step s = *step;

  /* Doing something new throws the redo steps away. If the file was saved
   * at one of those states we can never get back to it, hence -1. */
  if (h->saved > (long) h->pos)
    h->saved = -1;
  while (h->count > h->pos)
    free_step (h, &h->steps[--h->count]);

  if (h->count == h->cap)
    {
      size_t cap = h->cap ? h->cap * 2 : 64;
      Step *grown = realloc (h->steps, cap * sizeof *grown);
      if (grown == NULL)
        {
          free (s.before);
          free (s.after);
          return false;
        }
      h->steps = grown;
      h->cap = cap;
    }
  h->steps[h->count++] = s;
  h->bytes += step_bytes (&s);
  h->ram_bytes += step_bytes (&s);
  h->pos = h->count;

  /* Over the memory budget: forget the oldest steps, but always keep the
   * newest one even if it alone is bigger than the budget. */
  size_t drop = 0;
  while (h->bytes > h->max_bytes && drop + 1 < h->count)
    free_step (h, &h->steps[drop++]);
  if (drop > 0)
    {
      memmove (h->steps, h->steps + drop, (h->count - drop) * sizeof *h->steps);
      h->count -= drop;
      h->pos = h->count;
      h->saved = h->saved >= (long) drop ? h->saved - (long) drop : -1;
    }
  spill_oldest (h);
  return true;
}

bool
easel_history_push (EaselHistory *h, const EaselDocument *doc, const uint32_t *before, EaselRect area)
{
  /* Callers pass the area they *might* have touched, which is usually a lot
   * bigger than what really changed (a brush stamp over the same colour, say).
   * Compare against the old picture and store only the box that differs. */
  if (!easel_rect_clip (&area, doc->width, doc->height))
    return false;
  int x0 = area.x, y0 = area.y, x1 = area.x + area.width, y1 = area.y + area.height;
  int minx = x1, miny = y1, maxx = -1, maxy = -1;

  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++)
      {
        size_t i = (size_t) y * doc->width + x;
        if (doc->pixels[i] != before[i])
          {
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
          }
      }
  if (maxx < 0)
    return false;

  Step s = { { minx, miny, maxx - minx + 1, maxy - miny + 1 }, NULL, NULL, false, 0, 0, 0, 0, false, 0 };
  size_t n = (size_t) s.r.width * s.r.height;
  s.before = malloc (n * sizeof (uint32_t));
  s.after = malloc (n * sizeof (uint32_t));
  if (s.before == NULL || s.after == NULL)
    {
      free (s.before);
      free (s.after);
      return false;
    }
  for (int y = 0; y < s.r.height; y++)
    {
      size_t src = (size_t) (s.r.y + y) * doc->width + s.r.x;
      memcpy (s.before + (size_t) y * s.r.width, before + src, (size_t) s.r.width * 4);
      memcpy (s.after + (size_t) y * s.r.width, doc->pixels + src, (size_t) s.r.width * 4);
    }

  return append_step (h, &s);
}

bool
easel_history_can_undo (const EaselHistory *h)
{
  return h->pos > 0;
}

bool
easel_history_can_redo (const EaselHistory *h)
{
  return h->pos < h->count;
}

static void
blit (EaselDocument *doc, EaselRect r, const uint32_t *src)
{
  for (int y = 0; y < r.height; y++)
    memcpy (doc->pixels + (size_t) (r.y + y) * doc->width + r.x,
            src + (size_t) y * r.width, (size_t) r.width * 4);
}

bool
easel_history_undo (EaselHistory *h, EaselDocument *doc, EaselRect *changed)
{
  if (h->pos == 0)
    return false;
  Step *s = &h->steps[h->pos - 1];
  uint32_t *tmp;
  const uint32_t *px = step_pixels (h, s, false, &tmp);
  bool ok = px != NULL;

  if (ok && s->full)
    {
      ok = easel_document_replace (doc, s->bw, s->bh, px);
      if (ok && changed != NULL)
        *changed = (EaselRect) { 0, 0, s->bw, s->bh };
    }
  else if (ok)
    {
      blit (doc, s->r, px);
      if (changed != NULL)
        *changed = s->r;
    }
  free (tmp);
  if (ok)
    h->pos--;
  return ok;
}

bool
easel_history_redo (EaselHistory *h, EaselDocument *doc, EaselRect *changed)
{
  if (h->pos >= h->count)
    return false;
  Step *s = &h->steps[h->pos];
  uint32_t *tmp;
  const uint32_t *px = step_pixels (h, s, true, &tmp);
  bool ok = px != NULL;

  if (ok && s->full)
    {
      ok = easel_document_replace (doc, s->aw, s->ah, px);
      if (ok && changed != NULL)
        *changed = (EaselRect) { 0, 0, s->aw, s->ah };
    }
  else if (ok)
    {
      blit (doc, s->r, px);
      if (changed != NULL)
        *changed = s->r;
    }
  free (tmp);
  if (ok)
    h->pos++;
  return ok;
}

bool
easel_history_push_full (EaselHistory *h, const uint32_t *before, int before_w, int before_h,
                         const EaselDocument *doc)
{
  Step s = { { 0, 0, doc->width, doc->height }, NULL, NULL, true, before_w, before_h,
             doc->width, doc->height, false, 0 };
  size_t bn = (size_t) before_w * before_h, an = (size_t) doc->width * doc->height;

  s.before = malloc (bn * 4);
  s.after = malloc (an * 4);
  if (s.before == NULL || s.after == NULL)
    {
      free (s.before);
      free (s.after);
      return false;
    }
  memcpy (s.before, before, bn * 4);
  memcpy (s.after, doc->pixels, an * 4);
  return append_step (h, &s);
}

void
easel_history_mark_saved (EaselHistory *h)
{
  h->saved = (long) h->pos;
}

bool
easel_history_is_saved (const EaselHistory *h)
{
  return h->saved == (long) h->pos;
}
