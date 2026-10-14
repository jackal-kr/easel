/* Per-tool mouse cursors, laid out like the classic Paint ones: a precise
 * crosshair for shape/selection tools, and a small icon whose tip is the
 * hotspot for pencil, fill, pick colour, airbrush; a magnifier for zoom.
 * All drawn here with Cairo, aliasing off, so they stay crisp. */

#include "ui/cursors.h"

#define CURSOR_SIZE 32

typedef struct
{
  void (*draw) (cairo_t *cr);
  int hot_x, hot_y;
} CursorDef;

/* Black line with a white halo. */
static void
halo_line (cairo_t *cr, double x0, double y0, double x1, double y1, double w)
{
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_SQUARE);
  cairo_move_to (cr, x0, y0);
  cairo_line_to (cr, x1, y1);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_set_line_width (cr, w + 2);
  cairo_stroke_preserve (cr);
  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_set_line_width (cr, w);
  cairo_stroke (cr);
}

/* Closed polygon: white halo, fill, black edge. */
static void
poly (cairo_t *cr, const double *p, int n, double r, double g, double b)
{
  cairo_move_to (cr, p[0], p[1]);
  for (int i = 1; i < n; i++)
    cairo_line_to (cr, p[2 * i], p[2 * i + 1]);
  cairo_close_path (cr);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_set_line_width (cr, 3);
  cairo_stroke_preserve (cr);
  double x1, y1, x2, y2;
  cairo_path_extents (cr, &x1, &y1, &x2, &y2);
  cairo_pattern_t *grad = cairo_pattern_create_linear (x1, y1, x2, y2);
  cairo_pattern_add_color_stop_rgb (grad, 0, MIN (r + 0.25, 1), MIN (g + 0.25, 1), MIN (b + 0.25, 1));
  cairo_pattern_add_color_stop_rgb (grad, 1, r * 0.8, g * 0.8, b * 0.8);
  cairo_set_source (cr, grad);
  cairo_fill_preserve (cr);
  cairo_pattern_destroy (grad);
  cairo_set_source_rgb (cr, 0.1, 0.1, 0.12);
  cairo_set_line_width (cr, 1);
  cairo_stroke (cr);
}

/* Large crosshair centred on (16,16) with a small open centre. */
static void
draw_precise (cairo_t *cr)
{
  double c = 16.5;
  halo_line (cr, c, 2.5, c, 12.5, 1);
  halo_line (cr, c, 20.5, c, 30.5, 1);
  halo_line (cr, 2.5, c, 12.5, c, 1);
  halo_line (cr, 20.5, c, 30.5, c, 1);
}

/* Same, dotted: the brush, whose real footprint is previewed on the canvas. */
static void
draw_dotted (cairo_t *cr)
{
  double c = 16.5;
  static const double dash[] = { 1, 2 };
  cairo_set_dash (cr, dash, 2, 0);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_set_line_width (cr, 1);
  cairo_move_to (cr, c, 3); cairo_line_to (cr, c, 30);
  cairo_move_to (cr, 3, c); cairo_line_to (cr, 30, c);
  cairo_stroke (cr);
}

/* Pencil: tip at (13,23), body to the upper right. */
static void
draw_pencil (cairo_t *cr)
{
  static const double body[] = { 13.5, 23.5, 15, 18, 24, 9, 27.5, 12.5, 18.5, 21.5, 16, 22 };
  poly (cr, body, 6, 1.0, 0.9, 0.3);
  static const double lead[] = { 13.5, 23.5, 15, 18, 18.5, 21.5 };
  poly (cr, lead, 3, 0.35, 0.35, 0.35);
  static const double eraser[] = { 24, 9, 26, 7, 29.5, 10.5, 27.5, 12.5 };
  poly (cr, eraser, 4, 0.95, 0.45, 0.45);
}

/* Fill bucket pouring to the lower left; hotspot is the drip at (8,22). */
static void
draw_fill (cairo_t *cr)
{
  static const double bucket[] = { 11, 15, 18, 9, 28, 17, 22, 25, 17, 25 };
  poly (cr, bucket, 5, 0.75, 0.8, 0.9);
  static const double paint[] = { 11, 15, 7, 17, 6, 22, 9, 22, 10, 18, 13, 17 };
  poly (cr, paint, 6, 0.1, 0.2, 0.9);
  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_set_line_width (cr, 1);
  cairo_arc (cr, 17.5, 12, 3.5, G_PI * 1.1, G_PI * 1.9);
  cairo_stroke (cr);
}

/* Eyedropper: tip at (9,22), bulb at the upper right. */
static void
draw_dropper (cairo_t *cr)
{
  halo_line (cr, 9.5, 22.5, 12.5, 19.5, 1);
  static const double glass[] = { 12, 19, 14, 17, 24, 11, 21, 8, 15, 14 };
  poly (cr, glass, 5, 0.8, 0.9, 1.0);
  static const double bulb[] = { 21, 8, 23, 5, 28, 5, 28, 10, 25, 12 };
  poly (cr, bulb, 5, 0.15, 0.15, 0.2);
}

/* Magnifier: lens centred on the hotspot (16,16). */
static void
draw_magnifier (cairo_t *cr)
{
  halo_line (cr, 21.5, 21.5, 27.5, 27.5, 3);
  cairo_arc (cr, 16.5, 16.5, 7, 0, 2 * G_PI);
  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_set_line_width (cr, 4);
  cairo_stroke_preserve (cr);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_set_line_width (cr, 2);
  cairo_stroke_preserve (cr);
  cairo_set_source_rgba (cr, 0.75, 0.88, 1, 0.6);
  cairo_fill (cr);
}

/* Airbrush: can at the upper right, spray dots at the hotspot (7,22). */
static void
draw_airbrush (cairo_t *cr)
{
  static const double can[] = { 13, 16, 18, 11, 28, 17, 24, 24, 20, 24 };
  poly (cr, can, 5, 0.75, 0.8, 0.9);
  static const double cap[] = { 11, 18, 14, 14, 17, 16, 13, 20 };
  poly (cr, cap, 4, 0.3, 0.3, 0.35);
  static const double dots[][2] = { { 7, 22 }, { 5, 19 }, { 9, 19 }, { 4, 23 }, { 8, 25 },
                                    { 11, 23 }, { 6, 16 }, { 3, 20 } };
  cairo_set_source_rgb (cr, 0, 0, 0);
  for (guint i = 0; i < G_N_ELEMENTS (dots); i++)
    {
      cairo_rectangle (cr, dots[i][0] - 0.5, dots[i][1] - 0.5, 1, 1);
      cairo_fill (cr);
    }
}

static const CursorDef *
def_for (EaselTool tool)
{
  static const CursorDef precise = { draw_precise, 16, 16 };
  static const CursorDef dotted = { draw_dotted, 16, 16 };
  static const CursorDef pencil = { draw_pencil, 13, 23 };
  static const CursorDef fill = { draw_fill, 8, 22 };
  static const CursorDef dropper = { draw_dropper, 9, 22 };
  static const CursorDef magnifier = { draw_magnifier, 16, 16 };
  static const CursorDef airbrush = { draw_airbrush, 7, 22 };

  switch (tool)
    {
    case EASEL_TOOL_PENCIL:     return &pencil;
    case EASEL_TOOL_BRUSH:      return &dotted;
    case EASEL_TOOL_FILL:       return &fill;
    case EASEL_TOOL_PICK_COLOR: return &dropper;
    case EASEL_TOOL_MAGNIFIER:  return &magnifier;
    case EASEL_TOOL_AIRBRUSH:   return &airbrush;
    default:                    return &precise;
    }
}

static GdkCursor *
build_cursor (EaselTool tool)
{
  const CursorDef *def = def_for (tool);
  g_autoptr (GdkCursor) fallback = gdk_cursor_new_from_name ("crosshair", NULL);

  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                         CURSOR_SIZE, CURSOR_SIZE);
  cairo_t *cr = cairo_create (surface);
  /* Draw into a group, then composite a soft offset shadow under it. */
  cairo_push_group (cr);
  def->draw (cr);
  cairo_pattern_t *art = cairo_pop_group (cr);
  for (int i = 0; i < 2; i++)
    {
      cairo_save (cr);
      cairo_translate (cr, 1 + i * 0.5, 1 + i * 0.5);
      cairo_set_source_rgba (cr, 0, 0, 0, 0.18);
      cairo_mask (cr, art);
      cairo_restore (cr);
    }
  cairo_set_source (cr, art);
  cairo_paint (cr);
  cairo_pattern_destroy (art);
  cairo_destroy (cr);
  cairo_surface_flush (surface);

  int stride = cairo_image_surface_get_stride (surface);
  g_autoptr (GBytes) bytes = g_bytes_new (cairo_image_surface_get_data (surface),
                                          (gsize) stride * CURSOR_SIZE);
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  GdkMemoryFormat format = GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
#else
  GdkMemoryFormat format = GDK_MEMORY_A8R8G8B8_PREMULTIPLIED;
#endif
  g_autoptr (GdkTexture) texture = gdk_memory_texture_new (CURSOR_SIZE, CURSOR_SIZE, format,
                                                           bytes, (gsize) stride);
  cairo_surface_destroy (surface);

  return gdk_cursor_new_from_texture (texture, def->hot_x, def->hot_y, fallback);
}

GdkCursor *
easel_cursor_for_tool (EaselTool tool)
{
  static GdkCursor *cache[EASEL_N_TOOLS];

  g_return_val_if_fail (tool >= 0 && tool < EASEL_N_TOOLS, NULL);
  if (cache[tool] == NULL)
    cache[tool] = tool == EASEL_TOOL_PAN ? gdk_cursor_new_from_name ("grab", NULL) : build_cursor (tool);
  return cache[tool];
}
