#include "core/clip.h"

#include <stdlib.h>
#include <string.h>

EaselClip *
easel_clip_new (int width, int height)
{
  if (width <= 0 || height <= 0 || (uint64_t) width * (uint64_t) height > EASEL_MAX_PIXELS)
    return NULL;

  EaselClip *clip = calloc (1, sizeof *clip);
  if (clip == NULL)
    return NULL;
  clip->pixels = calloc ((size_t) width * height, sizeof (uint32_t));
  if (clip->pixels == NULL)
    {
      free (clip);
      return NULL;
    }
  clip->width = width;
  clip->height = height;
  return clip;
}

void
easel_clip_free (EaselClip *clip)
{
  if (clip == NULL)
    return;
  free (clip->pixels);
  free (clip->mask);
  free (clip);
}

EaselClip *
easel_clip_copy_from (const EaselDocument *doc, EaselRect area, const uint8_t *mask)
{
  EaselRect full = area;
  if (!easel_rect_clip (&full, doc->width, doc->height))
    return NULL;

  EaselClip *clip = easel_clip_new (full.width, full.height);
  if (clip == NULL)
    return NULL;

  for (int y = 0; y < full.height; y++)
    memcpy (clip->pixels + (size_t) y * full.width,
            doc->pixels + (size_t) (full.y + y) * doc->width + full.x, (size_t) full.width * 4);

  if (mask != NULL)
    {
      clip->mask = malloc ((size_t) full.width * full.height);
      if (clip->mask == NULL)
        {
          easel_clip_free (clip);
          return NULL;
        }
      /* `mask` is laid out for `area`; take the part that survived clipping. */
      for (int y = 0; y < full.height; y++)
        memcpy (clip->mask + (size_t) y * full.width,
                mask + (size_t) (full.y - area.y + y) * area.width + (full.x - area.x), (size_t) full.width);
    }
  return clip;
}

EaselRect
easel_clip_erase (EaselDocument *doc, EaselRect area, const uint8_t *mask, uint32_t color)
{
  EaselRect full = area;
  if (!easel_rect_clip (&full, doc->width, doc->height))
    return (EaselRect) { 0, 0, 0, 0 };

  /* `mask` is laid out for the unclipped `area`, hence the offsets. */
  for (int y = 0; y < full.height; y++)
    for (int x = 0; x < full.width; x++)
      {
        if (mask != NULL && !mask[(size_t) (full.y - area.y + y) * area.width + (full.x - area.x + x)])
          continue;
        doc->pixels[(size_t) (full.y + y) * doc->width + full.x + x] = color;
      }
  return full;
}

EaselRect
easel_clip_blit (EaselDocument *doc, const EaselClip *clip, int x, int y, bool transparent, uint32_t key)
{
  EaselRect dest = { x, y, clip->width, clip->height };
  EaselRect full = dest;
  if (!easel_rect_clip (&full, doc->width, doc->height))
    return (EaselRect) { 0, 0, 0, 0 };

  for (int j = 0; j < full.height; j++)
    for (int i = 0; i < full.width; i++)
      {
        size_t si = (size_t) (full.y - y + j) * clip->width + (full.x - x + i);
        if (clip->mask != NULL && !clip->mask[si])
          continue;
        uint32_t c = clip->pixels[si];
        if (transparent && (c == key || (c >> 24) == 0))
          continue;
        doc->pixels[(size_t) (full.y + j) * doc->width + full.x + i] = c;
      }
  return full;
}

EaselClip *
easel_clip_scaled (const EaselClip *clip, int w, int h)
{
  if (clip == NULL || w < 1 || h < 1)
    return NULL;
  EaselClip *out = easel_clip_new (w, h);
  if (out == NULL)
    return NULL;
  if (clip->mask != NULL)
    {
      out->mask = malloc ((size_t) w * h);
      if (out->mask == NULL)
        {
          easel_clip_free (out);
          return NULL;
        }
    }
  for (int y = 0; y < h; y++)
    {
      int sy = (int) ((long long) y * clip->height / h);
      for (int x = 0; x < w; x++)
        {
          int sx = (int) ((long long) x * clip->width / w);
          size_t si = (size_t) sy * clip->width + sx, di = (size_t) y * w + x;
          out->pixels[di] = clip->pixels[si];
          if (out->mask != NULL)
            out->mask[di] = clip->mask[si];
        }
    }
  return out;
}
