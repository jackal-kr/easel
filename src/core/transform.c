#include "core/transform.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Moves one cell (a pixel or a mask byte). Every routine below is generic over
 * the cell size, but a memcpy with a size only known at run time is not
 * inlined and costs a function call per pixel. The two sizes we actually use
 * get a fast path. */
static inline void
copy_cell (unsigned char *dst, const unsigned char *src, size_t elem)
{
  if (elem == 4)
    memcpy (dst, src, 4);
  else if (elem == 1)
    *dst = *src;
  else
    memcpy (dst, src, elem);
}

void
easel_flip (void *buf, int w, int h, size_t elem, int horizontal)
{
  unsigned char *b = buf;
  unsigned char tmp[16];

  if (elem > sizeof tmp)
    return;
  if (horizontal)
    {
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w / 2; x++)
          {
            unsigned char *l = b + ((size_t) y * w + x) * elem;
            unsigned char *r = b + ((size_t) y * w + (w - 1 - x)) * elem;
            copy_cell (tmp, l, elem);
            copy_cell (l, r, elem);
            copy_cell (r, tmp, elem);
          }
    }
  else
    {
      for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w; x++)
          {
            unsigned char *t = b + ((size_t) y * w + x) * elem;
            unsigned char *u = b + ((size_t) (h - 1 - y) * w + x) * elem;
            copy_cell (tmp, t, elem);
            copy_cell (t, u, elem);
            copy_cell (u, tmp, elem);
          }
    }
}

void *
easel_rotate (const void *src, int w, int h, size_t elem, int quarter_turns, int *nw, int *nh)
{
  int turns = ((quarter_turns % 4) + 4) % 4;
  int ow = (turns & 1) ? h : w, oh = (turns & 1) ? w : h;
  unsigned char *out = malloc ((size_t) ow * oh * elem);
  const unsigned char *s = src;

  if (out == NULL)
    return NULL;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        int dx, dy;
        switch (turns)
          {
          case 1: dx = h - 1 - y; dy = x; break;
          case 2: dx = w - 1 - x; dy = h - 1 - y; break;
          case 3: dx = y; dy = w - 1 - x; break;
          default: dx = x; dy = y; break;
          }
        copy_cell (out + ((size_t) dy * ow + dx) * elem, s + ((size_t) y * w + x) * elem, elem);
      }
  *nw = ow;
  *nh = oh;
  return out;
}

void *
easel_scale (const void *src, int w, int h, size_t elem, int nw, int nh)
{
  if (nw < 1 || nh < 1)
    return NULL;
  unsigned char *out = malloc ((size_t) nw * nh * elem);
  const unsigned char *s = src;

  if (out == NULL)
    return NULL;
  for (int y = 0; y < nh; y++)
    {
      int sy = (int) ((long long) y * h / nh);
      for (int x = 0; x < nw; x++)
        {
          int sx = (int) ((long long) x * w / nw);
          copy_cell (out + ((size_t) y * nw + x) * elem, s + ((size_t) sy * w + sx) * elem, elem);
        }
    }
  return out;
}

void *
easel_skew (const void *src, int w, int h, size_t elem, double degrees, int horizontal,
            const void *fill, int *nw, int *nh)
{
  double t = tan (fmax (-89.0, fmin (89.0, degrees)) * M_PI / 180.0);
  int along = horizontal ? h : w;               /* how many rows (or columns) slide */
  int extra = (int) ceil (fabs (t) * (along - 1));
  int ow = horizontal ? w + extra : w, oh = horizontal ? h : h + extra;
  unsigned char *out = malloc ((size_t) ow * oh * elem);
  const unsigned char *s = src;

  if (out == NULL)
    return NULL;
  /* Start with the whole result in the fill colour; the slanted copy of the
   * picture then leaves the triangles at the corners as they are. */
  for (size_t i = 0; i < (size_t) ow * oh; i++)
    copy_cell (out + i * elem, fill, elem);

  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        /* Every row (or column) k is shifted by a different amount, growing
         * in a straight line, which is what makes the picture lean. For a
         * positive angle the first row moves the most. */
        int k = horizontal ? y : x;
        int off = t >= 0 ? (int) lround (t * (along - 1 - k)) : (int) lround (-t * k);
        int dx = horizontal ? x + off : x, dy = horizontal ? y : y + off;

        copy_cell (out + ((size_t) dy * ow + dx) * elem, s + ((size_t) y * w + x) * elem, elem);
      }
  *nw = ow;
  *nh = oh;
  return out;
}

/* With premultiplied pixels the inverse of colour c is (alpha - c), not
 * (255 - c). Otherwise transparent areas would turn into light fog. */
void
easel_invert (uint32_t *pixels, size_t count)
{
  for (size_t i = 0; i < count; i++)
    {
      uint32_t c = pixels[i], a = c >> 24;
      uint32_t r = a - ((c >> 16) & 0xFF), g = a - ((c >> 8) & 0xFF), b = a - (c & 0xFF);

      pixels[i] = (a << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
    }
}

void
easel_black_and_white (uint32_t *pixels, size_t count)
{
  for (size_t i = 0; i < count; i++)
    {
      uint32_t c = pixels[i], a = c >> 24;
      uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
      /* Standard luma weights. The channels are premultiplied, so "at least
       * half bright" means luma >= a/2 rather than >= 128. */
      uint32_t lum = (299 * r + 587 * g + 114 * b) / 1000;

      pixels[i] = lum * 2 >= a ? (a << 24) | (a << 16) | (a << 8) | a : (a << 24);
    }
}
