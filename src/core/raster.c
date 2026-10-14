#include "core/raster.h"

#include <stdlib.h>

EaselRect
easel_raster_stamp (EaselDocument *doc, int cx, int cy, int size, uint32_t color)
{
  EaselRect dirty = { 0, 0, 0, 0 };

  if (size < 1)
    size = 1;
  if (size > EASEL_MAX_BRUSH)
    size = EASEL_MAX_BRUSH;

  int left = cx - size / 2;
  int top = cy - size / 2;
  int x0 = left < 0 ? 0 : left;
  int y0 = top < 0 ? 0 : top;
  int x1 = (left + size) > doc->width ? doc->width : (left + size);
  int y1 = (top + size) > doc->height ? doc->height : (top + size);
  if (x0 >= x1 || y0 >= y1)
    return dirty;

  /* A pixel belongs to the disc when its centre is within size/2 of the
   * brush centre. Working in doubled coordinates keeps this integer-only:
   * (2i+1-n)^2 + (2j+1-n)^2 <= n^2. */
  long limit = (long) size * size;
  for (int y = y0; y < y1; y++)
    {
      long dy = 2L * (y - top) + 1 - size;
      for (int x = x0; x < x1; x++)
        {
          long dx = 2L * (x - left) + 1 - size;
          if (dx * dx + dy * dy <= limit)
            doc->pixels[(size_t) y * (size_t) doc->width + (size_t) x] = color;
        }
    }

  dirty.x = x0;
  dirty.y = y0;
  dirty.width = x1 - x0;
  dirty.height = y1 - y0;
  return dirty;
}

EaselRect
easel_raster_stroke (EaselDocument *doc, int x0, int y0, int x1, int y1,
                     int size, uint32_t color)
{
  EaselRect dirty = { 0, 0, 0, 0 };

  int dx = abs (x1 - x0);
  int dy = -abs (y1 - y0);
  int sx = x0 < x1 ? 1 : -1;
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;

  for (;;)
    {
      dirty = easel_rect_union (dirty, easel_raster_stamp (doc, x0, y0, size, color));
      if (x0 == x1 && y0 == y1)
        break;

      int e2 = 2 * err;
      if (e2 >= dy)
        {
          err += dy;
          x0 += sx;
        }
      if (e2 <= dx)
        {
          err += dx;
          y0 += sy;
        }
    }

  return dirty;
}
