#include "core/syncbuf.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

EaselSyncBuf *
easel_syncbuf_new (void)
{
  EaselSyncBuf *buf = calloc (1, sizeof *buf);

  if (buf != NULL)
    buf->refs = 1;
  return buf;
}

EaselSyncBuf *
easel_syncbuf_ref (EaselSyncBuf *buf)
{
  atomic_fetch_add (&buf->refs, 1);
  return buf;
}

void
easel_syncbuf_unref (EaselSyncBuf *buf)
{
  if (buf != NULL && atomic_fetch_sub (&buf->refs, 1) == 1)
    {
      free (buf->data);
      free (buf);
    }
}

bool
easel_syncbuf_busy (EaselSyncBuf *buf)
{
  return atomic_load (&buf->refs) > 1;
}

void
easel_syncbuf_mark_stale (EaselSyncBuf *buf, EaselRect r)
{
  if (buf == NULL || buf->data == NULL || r.width <= 0 || r.height <= 0)
    return;
  buf->stale = easel_rect_union (buf->stale, r);
}

bool
easel_syncbuf_update (EaselSyncBuf *buf, const EaselDocument *doc)
{
  size_t bytes = (size_t) doc->width * doc->height * 4;

  if (buf->data == NULL || buf->width != doc->width || buf->height != doc->height)
    {
      uint32_t *fresh = malloc (bytes);

      if (fresh == NULL)
        return false;
      memcpy (fresh, doc->pixels, bytes);
      free (buf->data);
      buf->data = fresh;
      buf->width = doc->width;
      buf->height = doc->height;
    }
  else
    {
      /* Same size as before: patch up only the stale box, row by row. */
      EaselRect r = buf->stale;

      if (easel_rect_clip (&r, doc->width, doc->height))
        for (int y = r.y; y < r.y + r.height; y++)
          memcpy (&buf->data[(size_t) y * doc->width + r.x], &doc->pixels[(size_t) y * doc->width + r.x],
                  (size_t) r.width * sizeof (uint32_t));
    }
  buf->stale = (EaselRect) { 0, 0, 0, 0 };
  return true;
}
