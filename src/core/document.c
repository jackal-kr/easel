#include "core/document.h"

#include <stdlib.h>

/* 2^28 pixels = 1 GiB of ARGB. Refuse anything larger. */
#define EASEL_MAX_PIXELS ((uint64_t) 1 << 28)

EaselDocument *
easel_document_new (int width, int height, uint32_t fill)
{
  if (width <= 0 || height <= 0)
    return NULL;
  if ((uint64_t) width * (uint64_t) height > EASEL_MAX_PIXELS)
    return NULL;

  EaselDocument *doc = calloc (1, sizeof *doc);
  if (doc == NULL)
    return NULL;

  doc->pixels = malloc ((size_t) width * (size_t) height * sizeof (uint32_t));
  if (doc->pixels == NULL)
    {
      free (doc);
      return NULL;
    }

  doc->width = width;
  doc->height = height;
  easel_document_fill (doc, fill);
  return doc;
}

void
easel_document_free (EaselDocument *doc)
{
  if (doc == NULL)
    return;
  free (doc->pixels);
  free (doc);
}

void
easel_document_fill (EaselDocument *doc, uint32_t color)
{
  size_t n = (size_t) doc->width * (size_t) doc->height;
  for (size_t i = 0; i < n; i++)
    doc->pixels[i] = color;
}

uint32_t
easel_document_get_pixel (const EaselDocument *doc, int x, int y)
{
  if (x < 0 || y < 0 || x >= doc->width || y >= doc->height)
    return 0;
  return doc->pixels[(size_t) y * (size_t) doc->width + (size_t) x];
}

void
easel_document_set_pixel (EaselDocument *doc, int x, int y, uint32_t color)
{
  if (x < 0 || y < 0 || x >= doc->width || y >= doc->height)
    return;
  doc->pixels[(size_t) y * (size_t) doc->width + (size_t) x] = color;
}

EaselRect
easel_rect_union (EaselRect a, EaselRect b)
{
  if (a.width <= 0 || a.height <= 0)
    return b;
  if (b.width <= 0 || b.height <= 0)
    return a;

  int x0 = a.x < b.x ? a.x : b.x;
  int y0 = a.y < b.y ? a.y : b.y;
  int x1 = (a.x + a.width) > (b.x + b.width) ? (a.x + a.width) : (b.x + b.width);
  int y1 = (a.y + a.height) > (b.y + b.height) ? (a.y + a.height) : (b.y + b.height);
  return (EaselRect) { x0, y0, x1 - x0, y1 - y0 };
}
