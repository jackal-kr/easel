/* Whole-image and selection transforms: flip, rotate, stretch, skew, invert.
 * They work on raw buffers of `elem`-byte cells so one routine serves pixels
 * (4 bytes) and selection masks (1 byte). No GTK. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* In place. */
void easel_flip (void *buf, int w, int h, size_t elem, int horizontal);

/* Returns a new buffer (caller frees) turned clockwise by `quarter_turns`
 * (1, 2 or 3); *nw and *nh receive its size. NULL on failure. */
void *easel_rotate (const void *src, int w, int h, size_t elem, int quarter_turns, int *nw, int *nh);

/* Nearest-neighbour resize to nw x nh. New buffer, or NULL. */
void *easel_scale (const void *src, int w, int h, size_t elem, int nw, int nh);

/* Slants the picture by `degrees` (-89..89): horizontally, rows slide
 * sideways; vertically, columns slide up or down. The result is larger by the
 * slant; uncovered cells are copied from `fill` (elem bytes). New buffer. */
void *easel_skew (const void *src, int w, int h, size_t elem, double degrees, int horizontal,
                  const void *fill, int *nw, int *nh);

/* Inverts the colour of premultiplied ARGB pixels (alpha stays). */
void easel_invert (uint32_t *pixels, size_t count);

/* Reduces to two colors: pixels at least half as bright as white become white,
 * the rest black. Alpha is kept. */
void easel_black_and_white (uint32_t *pixels, size_t count);
