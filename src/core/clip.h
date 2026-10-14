/* A rectangular piece of picture lifted out of a document: the contents of a
 * selection. An optional mask (one byte per pixel, 0 = not part of it) lets a
 * free-form selection keep its shape. No GTK. */
#pragma once

#include "core/document.h"

typedef struct
{
  int       width, height;
  uint32_t *pixels; /* width * height */
  uint8_t  *mask;   /* NULL: every pixel belongs to the clip */
} EaselClip;

EaselClip *easel_clip_new (int width, int height);
void       easel_clip_free (EaselClip *clip);

/* Copies the pixels of `area` (clipped to the document) out of `doc`. `mask`,
 * if given, is area.width * area.height bytes and is copied too. */
EaselClip *easel_clip_copy_from (const EaselDocument *doc, EaselRect area, const uint8_t *mask);

/* Paints `color` over the masked pixels of `area` (all of it if no mask). */
EaselRect easel_clip_erase (EaselDocument *doc, EaselRect area, const uint8_t *mask, uint32_t color);

/* Draws the clip with its top-left at (x, y); parts outside are dropped. With
 * `transparent`, pixels equal to `key` (and fully transparent ones) are skipped. Returns the area changed. */
EaselRect easel_clip_blit (EaselDocument *doc, const EaselClip *clip, int x, int y,
                           bool transparent, uint32_t key);
