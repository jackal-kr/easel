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
  atomic_fetch_add ((_Atomic int *) &buf->refs, 1);
  return buf;
}

void
easel_syncbuf_unref (EaselSyncBuf *buf)
{
  if (buf != NULL && atomic_fetch_sub ((_Atomic int *) &buf->refs, 1) == 1)
    {
      free (buf->data);
      free (buf);
    }
}

bool
easel_syncbuf_busy (EaselSyncBuf *buf)
{
  return atomic_load ((_Atomic int *) &buf->refs) > 1;
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
  else if (buf->stale.width > 0 && buf->stale.height > 0)
    {
      int x0 = buf->stale.x < 0 ? 0 : buf->stale.x;
      int y0 = buf->stale.y < 0 ? 0 : buf->stale.y;
      int x1 = buf->stale.x + buf->stale.width > doc->width ? doc->width : buf->stale.x + buf->stale.width;
      int y1 = buf->stale.y + buf->stale.height > doc->height ? doc->height : buf->stale.y + buf->stale.height;

      for (int y = y0; y < y1 && x1 > x0; y++)
        memcpy (&buf->data[(size_t) y * doc->width + x0], &doc->pixels[(size_t) y * doc->width + x0],
                (size_t) (x1 - x0) * 4);
    }
  buf->stale = (EaselRect) { 0, 0, 0, 0 };
  return true;
}
