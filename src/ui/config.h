/* Constants shared across the UI, so each value is defined once. */
#pragma once

#include <gtk/gtk.h>
#include <stdint.h>

/* Largest picture side, in pixels (the engine also caps the pixel count). */
#define EASEL_MAX_DIMENSION 16384

#define EASEL_DEFAULT_FONT "Arial 12"

/* Undo history: the newest steps stay in memory up to EASEL_UNDO_RAM_MB, older
 * ones go to a temporary file. The limit counts both; past it the oldest
 * steps are dropped. */
#define EASEL_UNDO_LIMIT_MB 2048 /* default; Preferences can change it */
#define EASEL_UNDO_RAM_MB   64

/* Timers, in milliseconds. */
#define EASEL_CARET_BLINK_MS    530
#define EASEL_SPRAY_INTERVAL_MS 30
#define EASEL_THUMB_REFRESH_MS  250
#define EASEL_FRAME_REFRESH_MS  500

/* New text box width when the Text tool is just clicked, in pixels. */
#define EASEL_DEFAULT_TEXT_WIDTH 240

/* Longest side of the thumbnail window, in logical pixels. */
#define EASEL_THUMB_MAX 240

/* Zoom steps, in percent (Ctrl+wheel, zoom buttons). */
#define EASEL_ZOOM_STEPS { 10, 15, 20, 25, 33, 50, 67, 100, 200, 300, 400, 600, 800 }
#define EASEL_MIN_ZOOM 10
#define EASEL_MAX_ZOOM 800

/* Whole-number zoom factors offered by the Magnifier and Custom Zoom. */
#define EASEL_MAGNIFIER_LEVELS { 1, 2, 4, 6, 8 }

/* Pixels per inch assumed for the screen when converting units and printing. */
#define SCREEN_DPI 96.0

/* Colors: premultiplied 0xAARRGGBB <-> GdkRGBA and "#rrggbb". */
static inline GdkRGBA
easel_argb_to_rgba (uint32_t c, float alpha)
{
  return (GdkRGBA) { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, alpha };
}

static inline uint32_t
easel_rgba_to_argb (const GdkRGBA *rgba)
{
  return 0xFF000000u | ((uint32_t) (rgba->red * 255.0f + 0.5f) << 16)
         | ((uint32_t) (rgba->green * 255.0f + 0.5f) << 8) | (uint32_t) (rgba->blue * 255.0f + 0.5f);
}

static inline char *
easel_argb_to_hex (uint32_t c)
{
  return g_strdup_printf ("#%06x", c & 0xFFFFFF);
}
