/* The image itself: just a block of pixels. Nothing in core/ may include
 * GTK, Cairo or GLib, so all of it can be unit-tested without a display. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Pixel format: 0xAARRGGBB with the colour already multiplied by alpha
 * ("premultiplied"). On a little-endian CPU the bytes in memory come out as
 * B,G,R,A, which is exactly what Cairo ARGB32 and GDK's B8G8R8A8_PREMULTIPLIED
 * expect, so pixels can go to the screen without any conversion. */
#define EASEL_RGB(r, g, b) \
  ((uint32_t) 0xFF000000u | ((uint32_t) (r) << 16) | ((uint32_t) (g) << 8) | (uint32_t) (b))

/* Hard limit on the picture size: 2^28 pixels, which is 1 GiB of ARGB. */
#define EASEL_MAX_PIXELS ((uint64_t) 1 << 28)

typedef struct
{
  int x, y, width, height;
} EaselRect;

typedef struct
{
  int       width;
  int       height;
  uint32_t *pixels; /* width * height, row-major, no padding */
} EaselDocument;

/* Returns NULL for invalid sizes or allocation failure. */
EaselDocument *easel_document_new (int width, int height, uint32_t fill);
void           easel_document_free (EaselDocument *doc);
void           easel_document_fill (EaselDocument *doc, uint32_t color);

/* Out-of-bounds reads return 0; out-of-bounds writes are ignored. */
uint32_t easel_document_get_pixel (const EaselDocument *doc, int x, int y);
void     easel_document_set_pixel (EaselDocument *doc, int x, int y, uint32_t color);

/* Smallest rectangle holding both. An empty one (width or height 0) is ignored. */
EaselRect easel_rect_union (EaselRect a, EaselRect b);

/* Cuts *r down to the part inside a width x height picture. Returns false,
 * leaving *r alone, when nothing is left. */
bool easel_rect_clip (EaselRect *r, int width, int height);

/* Changes the size, keeping the top-left part of the picture; new area gets
 * `fill`. Returns false (document untouched) on invalid size or no memory. */
bool easel_document_resize (EaselDocument *doc, int width, int height, uint32_t fill);

/* Replaces size and contents with a copy of `pixels` (width * height). */
bool easel_document_replace (EaselDocument *doc, int width, int height, const uint32_t *pixels);
