#include "core/raster.h"

#include <stdlib.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

static int
clamp_size (int size)
{
  if (size < 1)
    return 1;
  if (size > EASEL_MAX_BRUSH)
    return EASEL_MAX_BRUSH;
  return size;
}

/* The size x size box whose top-left is (left, top), clipped to the image.
 * Width 0 when entirely outside. */
static EaselRect
clip_box (const EaselDocument *doc, int left, int top, int size)
{
  int x0 = left < 0 ? 0 : left;
  int y0 = top < 0 ? 0 : top;
  int x1 = (left + size) > doc->width ? doc->width : (left + size);
  int y1 = (top + size) > doc->height ? doc->height : (top + size);

  if (x0 >= x1 || y0 >= y1)
    return (EaselRect) { 0, 0, 0, 0 };
  return (EaselRect) { x0, y0, x1 - x0, y1 - y0 };
}

/* Is pixel (i, j) inside the n x n brush footprint? */
bool
easel_brush_covers (EaselBrushShape shape, int size, int i, int j)
{
  int n = clamp_size (size);

  if (i < 0 || j < 0 || i >= n || j >= n)
    return false;

  switch (shape)
    {
    case EASEL_BRUSH_CIRCLE:
      {
        /* Pixel centre within n/2 of the brush centre. Doubled coordinates
         * keep it integer-only: (2i+1-n)^2 + (2j+1-n)^2 <= n^2. */
        long dx = 2L * i + 1 - n;
        long dy = 2L * j + 1 - n;
        return dx * dx + dy * dy <= (long) n * n;
      }
    case EASEL_BRUSH_SQUARE:
      return true;
    case EASEL_BRUSH_SLASH:
      return i + j == n - 1;
    case EASEL_BRUSH_BACKSLASH:
      return i == j;
    }
  return false;
}

EaselRect
easel_raster_stamp (EaselDocument *doc, int cx, int cy, const EaselBrush *brush)
{
  int size = clamp_size (brush->size);
  int left = cx - size / 2;
  int top = cy - size / 2;
  EaselRect box = clip_box (doc, left, top, size);

  for (int y = box.y; y < box.y + box.height; y++)
    for (int x = box.x; x < box.x + box.width; x++)
      if (easel_brush_covers (brush->shape, size, x - left, y - top))
        doc->pixels[(size_t) y * (size_t) doc->width + (size_t) x] = brush->color;

  return box;
}

EaselRect
easel_raster_erase (EaselDocument *doc, int cx, int cy, const EaselEraser *eraser)
{
  int size = clamp_size (eraser->size);
  EaselRect box = clip_box (doc, cx - size / 2, cy - size / 2, size);

  for (int y = box.y; y < box.y + box.height; y++)
    for (int x = box.x; x < box.x + box.width; x++)
      {
        uint32_t *p = &doc->pixels[(size_t) y * (size_t) doc->width + (size_t) x];
        if (!eraser->color_only || *p == eraser->from_color)
          *p = eraser->background;
      }

  return box;
}

typedef EaselRect (*StampFunc) (EaselDocument *doc, int x, int y, const void *data);

/* Bresenham walk calling `stamp` at every point, both ends included. */
static EaselRect
walk_line (EaselDocument *doc, int x0, int y0, int x1, int y1, StampFunc stamp, const void *data)
{
  EaselRect dirty = { 0, 0, 0, 0 };
  int dx = abs (x1 - x0);
  int dy = -abs (y1 - y0);
  int sx = x0 < x1 ? 1 : -1;
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;

  for (;;)
    {
      dirty = easel_rect_union (dirty, stamp (doc, x0, y0, data));
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

static EaselRect
stamp_brush (EaselDocument *doc, int x, int y, const void *data)
{
  return easel_raster_stamp (doc, x, y, data);
}

static EaselRect
stamp_eraser (EaselDocument *doc, int x, int y, const void *data)
{
  return easel_raster_erase (doc, x, y, data);
}

EaselRect
easel_raster_stroke (EaselDocument *doc, int x0, int y0, int x1, int y1, const EaselBrush *brush)
{
  return walk_line (doc, x0, y0, x1, y1, stamp_brush, brush);
}

EaselRect
easel_raster_erase_stroke (EaselDocument *doc, int x0, int y0, int x1, int y1,
                           const EaselEraser *eraser)
{
  return walk_line (doc, x0, y0, x1, y1, stamp_eraser, eraser);
}

/* Growable stack of (x, y) seeds for the scanline fill. */
typedef struct
{
  int   *v;
  size_t len;
  size_t cap;
} SeedStack;

static bool
seed_push (SeedStack *s, int x, int y)
{
  if (s->len == s->cap)
    {
      size_t cap = s->cap ? s->cap * 2 : 256;
      int *v = realloc (s->v, cap * 2 * sizeof (int));
      if (v == NULL)
        return false;
      s->v = v;
      s->cap = cap;
    }
  s->v[s->len * 2] = x;
  s->v[s->len * 2 + 1] = y;
  s->len++;
  return true;
}

EaselRect
easel_raster_flood_fill (EaselDocument *doc, int x, int y, uint32_t color)
{
  EaselRect none = { 0, 0, 0, 0 };
  int w = doc->width;
  int h = doc->height;

  if (x < 0 || y < 0 || x >= w || y >= h)
    return none;

  uint32_t target = doc->pixels[(size_t) y * (size_t) w + (size_t) x];
  if (target == color)
    return none; /* also prevents an endless loop */

  SeedStack stack = { NULL, 0, 0 };
  int min_x = w, min_y = h, max_x = -1, max_y = -1;

  if (!seed_push (&stack, x, y))
    return none;

  while (stack.len > 0)
    {
      stack.len--;
      int cx = stack.v[stack.len * 2];
      int cy = stack.v[stack.len * 2 + 1];
      uint32_t *row = doc->pixels + (size_t) cy * (size_t) w;

      if (row[cx] != target)
        continue; /* already filled via another seed */

      int lx = cx, rx = cx;
      while (lx > 0 && row[lx - 1] == target)
        lx--;
      while (rx < w - 1 && row[rx + 1] == target)
        rx++;
      for (int i = lx; i <= rx; i++)
        row[i] = color;

      if (lx < min_x) min_x = lx;
      if (rx > max_x) max_x = rx;
      if (cy < min_y) min_y = cy;
      if (cy > max_y) max_y = cy;

      /* One seed per run of target pixels in the rows above and below. */
      for (int ny = cy - 1; ny <= cy + 1; ny += 2)
        {
          if (ny < 0 || ny >= h)
            continue;
          uint32_t *nrow = doc->pixels + (size_t) ny * (size_t) w;
          int i = lx;
          while (i <= rx)
            {
              if (nrow[i] != target)
                {
                  i++;
                  continue;
                }
              if (!seed_push (&stack, i, ny))
                goto done; /* out of memory: keep what is filled so far */
              while (i <= rx && nrow[i] == target)
                i++;
            }
        }
    }

done:
  free (stack.v);
  if (max_x < 0)
    return none;
  return (EaselRect) { min_x, min_y, max_x - min_x + 1, max_y - min_y + 1 };
}

EaselRect
easel_raster_line (EaselDocument *doc, int x0, int y0, int x1, int y1, int width, uint32_t color)
{
  EaselBrush brush = { EASEL_BRUSH_CIRCLE, width, color };
  return easel_raster_stroke (doc, x0, y0, x1, y1, &brush);
}

/* Is the pixel (x, y) inside the shape filling box (bx, by, bw, bh)? Done in
 * doubled coordinates so pixel centres are exact integers. */
static bool
shape_contains (EaselShape shape, int bx, int by, int bw, int bh, int r, int x, int y)
{
  if (x < bx || y < by || x >= bx + bw || y >= by + bh)
    return false;

  if (shape == EASEL_SHAPE_ELLIPSE)
    {
      int64_t dx = 2 * (int64_t) x + 1 - (2 * (int64_t) bx + bw);
      int64_t dy = 2 * (int64_t) y + 1 - (2 * (int64_t) by + bh);
      return dx * dx * bh * bh + dy * dy * (int64_t) bw * bw <= (int64_t) bw * bw * bh * bh;
    }

  if (r <= 0)
    return true;

  int64_t px = 2 * (int64_t) x + 1, py = 2 * (int64_t) y + 1;
  int64_t lx = 2 * (int64_t) (bx + r), rx = 2 * (int64_t) (bx + bw - r);
  int64_t ty = 2 * (int64_t) (by + r), bt = 2 * (int64_t) (by + bh - r);
  int64_t dx = px < lx ? lx - px : (px > rx ? px - rx : 0);
  int64_t dy = py < ty ? ty - py : (py > bt ? py - bt : 0);
  return dx * dx + dy * dy <= 4 * (int64_t) r * r;
}

EaselRect
easel_raster_shape (EaselDocument *doc, EaselShape shape, int x0, int y0, int x1, int y1,
                    int line_width, EaselFillStyle style, uint32_t outline_color, uint32_t fill_color)
{
  EaselRect none = { 0, 0, 0, 0 };
  int bx = MIN (x0, x1), by = MIN (y0, y1);
  int bw = abs (x1 - x0) + 1, bh = abs (y1 - y0) + 1;
  int w = line_width < 1 ? 1 : (line_width > EASEL_MAX_BRUSH ? EASEL_MAX_BRUSH : line_width);
  int r = 0;

  if (shape == EASEL_SHAPE_ROUNDED_RECT)
    r = MIN (8, MIN (bw, bh) / 2);

  /* Interior (where the border stops). Empty when the border swallows it. */
  int ix = bx + w, iy = by + w, iw = bw - 2 * w, ih = bh - 2 * w;
  int ir = r > w ? r - w : 0;
  bool has_inner = iw > 0 && ih > 0;

  /* Clip the loop to the image. */
  int left = MAX (bx, 0), top = MAX (by, 0);
  int right = MIN ((int64_t) bx + bw, doc->width), bottom = MIN ((int64_t) by + bh, doc->height);
  if (left >= right || top >= bottom)
    return none;

  for (int y = top; y < bottom; y++)
    for (int x = left; x < right; x++)
      {
        if (!shape_contains (shape, bx, by, bw, bh, r, x, y))
          continue;

        bool inner = has_inner && shape_contains (shape, ix, iy, iw, ih, ir, x, y);
        uint32_t color;

        if (style == EASEL_FILL_SOLID)
          color = outline_color;
        else if (!inner)
          color = outline_color;
        else if (style == EASEL_FILL_OUTLINE_AND_FILL)
          color = fill_color;
        else
          continue;

        doc->pixels[(size_t) y * doc->width + x] = color;
      }

  return (EaselRect) { left, top, right - left, bottom - top };
}
