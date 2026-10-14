/* The image document: a plain pixel buffer. No GTK, no Cairo, no GLib. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Pixels are 0xAARRGGBB, premultiplied. In memory on little-endian this is
 * B,G,R,A bytes, which matches Cairo ARGB32 and GDK_MEMORY_B8G8R8A8_PREMULTIPLIED. */
#define EASEL_RGB(r, g, b) \
  ((uint32_t) 0xFF000000u | ((uint32_t) (r) << 16) | ((uint32_t) (g) << 8) | (uint32_t) (b))

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

EaselRect easel_rect_union (EaselRect a, EaselRect b);
