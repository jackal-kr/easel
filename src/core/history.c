#include "core/history.h"

#include <stdlib.h>
#include <string.h>

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
} Step;

struct _EaselHistory
{
  Step  *steps;
  size_t count;    /* steps stored */
  size_t cap;      /* allocated slots in `steps` */
  size_t pos;      /* steps[0..pos) are applied, steps[pos..count) are what redo would bring back */
  size_t bytes;
  size_t max_bytes;
  long   saved;    /* value of pos at the last save; -1 when that state is gone for good */
};

static size_t
step_bytes (const Step *s)
{
  if (s->full)
    return ((size_t) s->bw * s->bh + (size_t) s->aw * s->ah) * sizeof (uint32_t);
  return (size_t) s->r.width * s->r.height * sizeof (uint32_t) * 2;
}

static void
free_step (EaselHistory *h, Step *s)
{
  h->bytes -= step_bytes (s);
  free (s->before);
  free (s->after);
}

EaselHistory *
easel_history_new (size_t max_bytes)
{
  EaselHistory *h = calloc (1, sizeof *h);
  if (h != NULL)
    h->max_bytes = max_bytes;
  return h;
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

  Step s = { { minx, miny, maxx - minx + 1, maxy - miny + 1 }, NULL, NULL, false, 0, 0, 0, 0 };
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
  Step *s = &h->steps[--h->pos];
  if (s->full)
    {
      if (!easel_document_replace (doc, s->bw, s->bh, s->before))
        {
          h->pos++;
          return false;
        }
      if (changed != NULL)
        *changed = (EaselRect) { 0, 0, s->bw, s->bh };
      return true;
    }
  blit (doc, s->r, s->before);
  if (changed != NULL)
    *changed = s->r;
  return true;
}

bool
easel_history_redo (EaselHistory *h, EaselDocument *doc, EaselRect *changed)
{
  if (h->pos >= h->count)
    return false;
  Step *s = &h->steps[h->pos];
  if (s->full)
    {
      if (!easel_document_replace (doc, s->aw, s->ah, s->after))
        return false;
      h->pos++;
      if (changed != NULL)
        *changed = (EaselRect) { 0, 0, s->aw, s->ah };
      return true;
    }
  h->pos++;
  blit (doc, s->r, s->after);
  if (changed != NULL)
    *changed = s->r;
  return true;
}

bool
easel_history_push_full (EaselHistory *h, const uint32_t *before, int before_w, int before_h,
                         const EaselDocument *doc)
{
  Step s = { { 0, 0, doc->width, doc->height }, NULL, NULL, true, before_w, before_h,
             doc->width, doc->height };
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
