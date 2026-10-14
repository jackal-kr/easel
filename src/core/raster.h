/* Hard-edged (non-antialiased) drawing primitives, XP Paint style. */
#pragma once

#include "core/document.h"

#define EASEL_MAX_BRUSH 256

/* Stamps a round brush of diameter `size` (clamped to 1..EASEL_MAX_BRUSH)
 * centred on (cx, cy). Returns the clipped bounding box that was touched
 * (width 0 if nothing was). */
EaselRect easel_raster_stamp (EaselDocument *doc, int cx, int cy, int size, uint32_t color);

/* Bresenham line from (x0,y0) to (x1,y1) with the round brush stamped at
 * every step. Both endpoints are drawn. Returns the touched bounding box. */
EaselRect easel_raster_stroke (EaselDocument *doc, int x0, int y0, int x1, int y1,
                               int size, uint32_t color);
