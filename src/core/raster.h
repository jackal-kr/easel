/* Hard-edged (non-antialiased) drawing primitives, XP Paint style.
 * Every function returns the clipped bounding box it touched (width 0 when
 * nothing was drawn) so the UI can invalidate only that region. */
#pragma once

#include "core/document.h"

#define EASEL_MAX_BRUSH 256

typedef enum
{
  EASEL_BRUSH_CIRCLE,
  EASEL_BRUSH_SQUARE,
  EASEL_BRUSH_SLASH,     /* "/" diagonal line */
  EASEL_BRUSH_BACKSLASH, /* "\" diagonal line */
} EaselBrushShape;

typedef struct
{
  EaselBrushShape shape;
  int             size; /* clamped to 1..EASEL_MAX_BRUSH */
  uint32_t        color;
} EaselBrush;

/* Eraser: a size x size square. Normally it paints `background`; with
 * color_only it only replaces pixels equal to `from_color` (the classic
 * right-button "color eraser"). */
typedef struct
{
  int      size;
  uint32_t background;
  bool     color_only;
  uint32_t from_color;
} EaselEraser;

/* Is pixel (i, j) of the size x size footprint box part of the brush shape?
 * Box origin is (cx - size/2, cy - size/2) for a stamp centred on (cx, cy).
 * The UI uses this to preview exactly what a stamp will paint. */
bool easel_brush_covers (EaselBrushShape shape, int size, int i, int j);

/* One brush footprint centred on (cx, cy). */
EaselRect easel_raster_stamp (EaselDocument *doc, int cx, int cy, const EaselBrush *brush);

/* Bresenham line with the brush stamped at every step; both ends included. */
EaselRect easel_raster_stroke (EaselDocument *doc, int x0, int y0, int x1, int y1,
                               const EaselBrush *brush);

EaselRect easel_raster_erase (EaselDocument *doc, int cx, int cy, const EaselEraser *eraser);
EaselRect easel_raster_erase_stroke (EaselDocument *doc, int x0, int y0, int x1, int y1,
                                     const EaselEraser *eraser);

/* 4-connected flood fill of the exact colour found at (x, y). Does nothing if
 * the point is outside the image or already has `color`. */
EaselRect easel_raster_flood_fill (EaselDocument *doc, int x, int y, uint32_t color);

/* Straight line `width` pixels thick (round pen), both ends included. */
EaselRect easel_raster_line (EaselDocument *doc, int x0, int y0, int x1, int y1,
                             int width, uint32_t color);

typedef enum
{
  EASEL_SHAPE_RECT,
  EASEL_SHAPE_ROUNDED_RECT, /* corner radius min(8, half the shorter side) */
  EASEL_SHAPE_ELLIPSE,
} EaselShape;

typedef enum
{
  EASEL_FILL_OUTLINE,          /* border only */
  EASEL_FILL_OUTLINE_AND_FILL, /* border in outline_color, inside in fill_color */
  EASEL_FILL_SOLID,            /* no border; the whole shape in outline_color */
} EaselFillStyle;

/* Shape inscribed in the box spanned by the two corners (inclusive, any
 * order). The border grows inward and is `line_width` pixels thick. */
EaselRect easel_raster_shape (EaselDocument *doc, EaselShape shape,
                              int x0, int y0, int x1, int y1, int line_width,
                              EaselFillStyle style, uint32_t outline_color, uint32_t fill_color);

typedef struct { int x, y; } EaselPoint;

/* Filled polygon (even-odd rule, pixel centres), then its border according
 * to `style`, like the shape tools. Needs n >= 2 (2 points = a line). */
EaselRect easel_raster_polygon (EaselDocument *doc, const EaselPoint *pts, int n, int line_width,
                                EaselFillStyle style, uint32_t outline_color, uint32_t fill_color);

/* Open polyline through the points. */
EaselRect easel_raster_polyline (EaselDocument *doc, const EaselPoint *pts, int n, int line_width,
                                 uint32_t color);

/* Cubic Bezier from p0 to p3 with control points c1, c2, drawn as a polyline. */
EaselRect easel_raster_curve (EaselDocument *doc, EaselPoint p0, EaselPoint c1, EaselPoint c2,
                              EaselPoint p3, int line_width, uint32_t color);

/* Airbrush: `count` random pixels inside a circle of the given diameter. The
 * generator state `*rng` must be non-zero and is advanced. */
EaselRect easel_raster_spray (EaselDocument *doc, int cx, int cy, int diameter, uint32_t color,
                              int count, uint32_t *rng);
