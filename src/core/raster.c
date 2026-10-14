#include "core/raster.h"

#include <math.h>
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

/* A size x size box at (left, top), cut down to the picture. Comes back with
 * zero width if it is completely outside. */
static EaselRect
clip_box (const EaselDocument *doc, int left, int top, int size)
{
  EaselRect box = { left, top, size, size };

  if (!easel_rect_clip (&box, doc->width, doc->height))
    return (EaselRect) { 0, 0, 0, 0 };
  return box;
}

/* Is pixel (i, j) of the n x n brush box painted? The canvas uses this same
 * function for the hover preview, so what you see is what you get. */
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
        /* Painted if the pixel's centre is within n/2 of the brush centre.
         * Doubling every coordinate gets rid of the halves, so it stays pure
         * integer maths (no float rounding surprises at the circle's edge):
         * (2i+1-n)^2 + (2j+1-n)^2 <= n^2 */
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

/* Classic Bresenham: calls visit() for every pixel from (x0, y0) to (x1, y1),
 * both ends included. All the line-ish things below (pencil strokes, the
 * eraser, shape outlines, free-form selection edges) go through this one. */
typedef void (*VisitFunc) (int x, int y, void *ctx);

static void
bresenham (int x0, int y0, int x1, int y1, VisitFunc visit, void *ctx)
{
  int dx = abs (x1 - x0);
  int dy = -abs (y1 - y0);
  int sx = x0 < x1 ? 1 : -1;
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;

  for (;;)
    {
      visit (x0, y0, ctx);
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
}

/* Stamping something (brush, eraser) along a line, collecting the dirty box. */
typedef EaselRect (*StampFunc) (EaselDocument *doc, int x, int y, const void *data);

typedef struct
{
  EaselDocument *doc;
  StampFunc      stamp;
  const void    *data;
  EaselRect      dirty;
} StampWalk;

static void
visit_stamp (int x, int y, void *ctx)
{
  StampWalk *w = ctx;

  w->dirty = easel_rect_union (w->dirty, w->stamp (w->doc, x, y, w->data));
}

static EaselRect
walk_line (EaselDocument *doc, int x0, int y0, int x1, int y1, StampFunc stamp, const void *data)
{
  StampWalk w = { doc, stamp, data, { 0, 0, 0, 0 } };

  bresenham (x0, y0, x1, y1, visit_stamp, &w);
  return w.dirty;
}

/* Thin adapters so the stamp functions fit the StampFunc signature. Casting
 * the function pointers instead would be undefined behaviour in C. */
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

/* Flood fill needs a stack of places still to look at. A recursive fill would
 * blow the real stack on a big empty picture, so it lives on the heap. */
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
    return none; /* nothing to do, and without this check we'd never stop */

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
        continue; /* an earlier seed already covered this run */

      /* Grow to the full run of target-coloured pixels on this row. */
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

      /* Look at the rows above and below the run we just painted. Each
       * separate stretch of target colour there gets one seed. This is the
       * "scanline" trick: far fewer pushes than one per pixel. */
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

/* Is pixel (x, y) inside the shape that fills the box (bx, by, bw, bh)?
 * `r` is the corner radius for rounded rectangles. Same doubled-coordinate
 * trick as the brush: pixel centres become whole numbers. */
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

  /* The border is drawn inside the box and is `w` thick, so the inner shape
   * is the same shape shrunk by w on every side. A pixel is border if it is
   * in the outer shape but not the inner one. With a thick line there may be
   * no inner shape left at all. */
  int ix = bx + w, iy = by + w, iw = bw - 2 * w, ih = bh - 2 * w;
  int ir = r > w ? r - w : 0;
  bool has_inner = iw > 0 && ih > 0;

  /* Only walk the part of the box that is on the picture. */
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

EaselRect
easel_raster_polyline (EaselDocument *doc, const EaselPoint *pts, int n, int line_width, uint32_t color)
{
  EaselRect r = { 0, 0, 0, 0 };

  if (n == 1)
    return easel_raster_line (doc, pts[0].x, pts[0].y, pts[0].x, pts[0].y, line_width, color);
  for (int i = 0; i + 1 < n; i++)
    r = easel_rect_union (r, easel_raster_line (doc, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y,
                                          line_width, color));
  return r;
}

static int
compare_double (const void *a, const void *b)
{
  double x = *(const double *) a, y = *(const double *) b;
  return (x > y) - (x < y);
}

/* Where the polygon's edges cross the horizontal line through the middle of
 * pixel row `cy`, sorted left to right. Even-odd rule: pixels between the 1st
 * and 2nd crossing are inside, between the 3rd and 4th, and so on.
 * `xs` needs room for n values; returns how many were written. */
static int
scan_crossings (const EaselPoint *pts, int n, double cy, double *xs)
{
  int count = 0;

  for (int i = 0; i < n; i++)
    {
      /* Vertices sit at pixel centres, like the outline pixels do. */
      EaselPoint a = pts[i], b = pts[(i + 1) % n];
      double ay = a.y + 0.5, by = b.y + 0.5;

      if ((ay <= cy) == (by <= cy))
        continue; /* this edge doesn't cross our line (horizontal ones never do) */
      xs[count++] = a.x + 0.5 + ((cy - ay) / (by - ay)) * (b.x - a.x);
    }
  qsort (xs, (size_t) count, sizeof *xs, compare_double);
  return count;
}

static EaselRect
fill_polygon (EaselDocument *doc, const EaselPoint *pts, int n, uint32_t color)
{
  EaselRect none = { 0, 0, 0, 0 };
  int min_y = pts[0].y, max_y = pts[0].y, min_x = pts[0].x, max_x = pts[0].x;

  for (int i = 1; i < n; i++)
    {
      min_y = MIN (min_y, pts[i].y); max_y = MAX (max_y, pts[i].y);
      min_x = MIN (min_x, pts[i].x); max_x = MAX (max_x, pts[i].x);
    }
  int top = MAX (min_y, 0), bottom = MIN (max_y, doc->height - 1);
  if (top > bottom)
    return none;

  double *xs = malloc ((size_t) n * sizeof *xs);
  if (xs == NULL)
    return none;

  for (int y = top; y <= bottom; y++)
    {
      int count = scan_crossings (pts, n, y + 0.5, xs);

      for (int k = 0; k + 1 < count; k += 2)
        {
          int from = (int) ceil (xs[k] - 0.5), to = (int) ceil (xs[k + 1] - 0.5) - 1;
          from = MAX (from, 0);
          to = MIN (to, doc->width - 1);
          for (int x = from; x <= to; x++)
            doc->pixels[(size_t) y * doc->width + x] = color;
        }
    }
  free (xs);

  int left = MAX (min_x, 0), right = MIN (max_x, doc->width - 1);
  if (left > right)
    return none;
  return (EaselRect) { left, top, right - left + 1, bottom - top + 1 };
}

EaselRect
easel_raster_polygon (EaselDocument *doc, const EaselPoint *pts, int n, int line_width,
                      EaselFillStyle style, uint32_t outline_color, uint32_t fill_color)
{
  EaselRect r = { 0, 0, 0, 0 };

  if (n < 1)
    return r;
  if (n >= 3 && style != EASEL_FILL_OUTLINE)
    r = fill_polygon (doc, pts, n,
                      style == EASEL_FILL_SOLID ? outline_color : fill_color);

  /* Border on top of the fill. For SOLID it is the same colour as the fill,
   * which just tidies up the edge pixels. */
  r = easel_rect_union (r, easel_raster_polyline (doc, pts, n, line_width, outline_color));
  if (n >= 3)
    r = easel_rect_union (r, easel_raster_line (doc, pts[n - 1].x, pts[n - 1].y, pts[0].x, pts[0].y,
                                          line_width, outline_color));
  return r;
}

EaselRect
easel_raster_curve (EaselDocument *doc, EaselPoint p0, EaselPoint c1, EaselPoint c2, EaselPoint p3,
                    int line_width, uint32_t color)
{
  double len = hypot (c1.x - p0.x, c1.y - p0.y) + hypot (c2.x - c1.x, c2.y - c1.y)
               + hypot (p3.x - c2.x, p3.y - c2.y);
  /* Enough segments that the polyline looks smooth: about one per pixel of
   * control-polygon length, but never fewer than 8 or more than 1024. */
  int steps = MAX (8, MIN ((int) len, 1024));
  EaselRect r = { 0, 0, 0, 0 };
  EaselPoint prev = p0;

  for (int i = 1; i <= steps; i++)
    {
      double t = (double) i / steps, u = 1 - t;
      double bx = u * u * u * p0.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * p3.x;
      double by = u * u * u * p0.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * p3.y;
      EaselPoint cur = { (int) lround (bx), (int) lround (by) };

      if (cur.x == prev.x && cur.y == prev.y && i != steps)
        continue; /* didn't move a whole pixel yet */
      r = easel_rect_union (r, easel_raster_line (doc, prev.x, prev.y, cur.x, cur.y, line_width, color));
      prev = cur;
    }
  return r;
}

/* xorshift32: tiny, fast, and good enough for spraying dots. Keeping the
 * state in the caller means no hidden global and repeatable tests. */
static uint32_t
next_random (uint32_t *rng)
{
  uint32_t x = *rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return *rng = x;
}

EaselRect
easel_raster_spray (EaselDocument *doc, int cx, int cy, int diameter, uint32_t color, int count,
                    uint32_t *rng)
{
  int d = diameter < 1 ? 1 : (diameter > EASEL_MAX_BRUSH ? EASEL_MAX_BRUSH : diameter);
  int left = cx - d / 2, top = cy - d / 2;

  if (*rng == 0)
    *rng = 0x9E3779B9u;
  for (int k = 0; k < count; k++)
    {
      int i = (int) (next_random (rng) % (uint32_t) d);
      int j = (int) (next_random (rng) % (uint32_t) d);
      int x = left + i, y = top + j;

      if (!easel_brush_covers (EASEL_BRUSH_CIRCLE, d, i, j))
        continue;
      if (x >= 0 && y >= 0 && x < doc->width && y < doc->height)
        doc->pixels[(size_t) y * doc->width + x] = color;
    }

  int x0 = MAX (left, 0), y0 = MAX (top, 0);
  int x1 = MIN (left + d, doc->width), y1 = MIN (top + d, doc->height);
  if (x0 >= x1 || y0 >= y1)
    return (EaselRect) { 0, 0, 0, 0 };
  return (EaselRect) { x0, y0, x1 - x0, y1 - y0 };
}

typedef struct
{
  uint8_t  *mask;
  EaselRect box;
} MaskPlot;

static void
plot_mask (int x, int y, void *ctx)
{
  MaskPlot *m = ctx;
  int mx = x - m->box.x, my = y - m->box.y;

  if (mx >= 0 && my >= 0 && mx < m->box.width && my < m->box.height)
    m->mask[(size_t) my * m->box.width + mx] = 1;
}

uint8_t *
easel_raster_polygon_mask (const EaselPoint *pts, int n, EaselRect box)
{
  if (n < 1 || box.width <= 0 || box.height <= 0)
    return NULL;

  uint8_t *mask = calloc ((size_t) box.width * box.height, 1);
  double *xs = malloc ((size_t) (n > 0 ? n : 1) * sizeof *xs);
  if (mask == NULL || xs == NULL)
    {
      free (mask);
      free (xs);
      return NULL;
    }

  /* Inside of the outline: same even-odd scanline fill as fill_polygon(). */
  for (int row = 0; row < box.height; row++)
    {
      int count = scan_crossings (pts, n, box.y + row + 0.5, xs);

      for (int k = 0; k + 1 < count; k += 2)
        {
          int from = MAX ((int) ceil (xs[k] - 0.5) - box.x, 0);
          int to = MIN ((int) ceil (xs[k + 1] - 0.5) - 1 - box.x, box.width - 1);
          for (int x = from; x <= to; x++)
            mask[(size_t) row * box.width + x] = 1;
        }
    }
  free (xs);

  /* The pixels the outline itself passes through belong to the selection too. */
  MaskPlot plot = { mask, box };

  for (int i = 0; i < n; i++)
    bresenham (pts[i].x, pts[i].y, pts[(i + 1) % n].x, pts[(i + 1) % n].y, plot_mask, &plot);
  return mask;
}

EaselRect
easel_raster_gradient (EaselDocument *doc, EaselRect region, const uint8_t *mask,
                       int x0, int y0, int x1, int y1, EaselGradientStyle style,
                       uint32_t c0, uint32_t c1)
{
  EaselRect none = { 0, 0, 0, 0 };
  double dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy, len = sqrt (len2);
  int left = MAX (region.x, 0), top = MAX (region.y, 0);
  int right = MIN (region.x + region.width, doc->width), bottom = MIN (region.y + region.height, doc->height);

  if (len2 == 0 || left >= right || top >= bottom)
    return none;

  for (int y = top; y < bottom; y++)
    for (int x = left; x < right; x++)
      {
        if (mask != NULL && !mask[(size_t) (y - region.y) * region.width + (x - region.x)])
          continue;

        double px = x + 0.5 - (x0 + 0.5), py = y + 0.5 - (y0 + 0.5), t;
        if (style == EASEL_GRADIENT_RADIAL)
          t = sqrt (px * px + py * py) / len;
        else
          {
            t = (px * dx + py * dy) / len2;
            if (style == EASEL_GRADIENT_REFLECTED)
              t = fabs (t);
          }
        t = t < 0 ? 0 : t > 1 ? 1 : t;

        /* Blend each of the four channels (A, R, G, B) on its own. Because
         * pixels are premultiplied, blending the raw values is correct. */
        uint32_t out = 0;
        for (int sh = 0; sh <= 24; sh += 8)
          {
            double a = (c0 >> sh) & 0xFF, b = (c1 >> sh) & 0xFF;
            out |= (uint32_t) (a + (b - a) * t + 0.5) << sh;
          }
        doc->pixels[(size_t) y * doc->width + x] = out;
      }
  return (EaselRect) { left, top, right - left, bottom - top };
}
