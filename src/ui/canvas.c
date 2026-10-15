#include "ui/canvas-private.h"
#include "ui/settings.h"

G_DEFINE_FINAL_TYPE (EaselCanvas, easel_canvas, GTK_TYPE_WIDGET)

guint canvas_signals[N_SIGNALS];

/* At 100% one picture pixel is one *device* pixel, also on a HiDPI screen
 * where GTK's logical pixels are 2x bigger. That's why we divide by the scale
 * factor: GTK sizes are in logical pixels, the picture is not. */
double
canvas_logical_per_pixel (EaselCanvas *self)
{
  return self->zoom / 100.0 / gtk_widget_get_scale_factor (GTK_WIDGET (self));
}

static void
to_image (EaselCanvas *self, double wx, double wy, int *ix, int *iy)
{
  double s = canvas_logical_per_pixel (self);
  *ix = (int) floor (wx / s);
  *iy = (int) floor (wy / s);
}

/* Gives you a frozen copy of the picture as it is now. Undo needs it ("what
 * did it look like before this stroke?") and so do the shape previews.
 *
 * Copying the full picture at the start of every stroke would hurt on big
 * images, so we keep one long-lived copy (`shadow`) and just patch the
 * rectangles that changed since the last time. It can only be lent out to one
 * caller at a time; if it's already out, you get a one-off full copy.
 * Hand it back with canvas_snapshot_drop(). */
uint32_t *
canvas_snapshot_take (EaselCanvas *self)
{
  if (self->shadow_taken)
    return g_memdup2 (self->doc->pixels, (gsize) self->doc->width * self->doc->height * 4);
  if (self->shadow == NULL)
    self->shadow = easel_syncbuf_new ();
  easel_syncbuf_update (self->shadow, self->doc);
  self->shadow_taken = TRUE;
  return self->shadow->data;
}

void
canvas_snapshot_drop (EaselCanvas *self, uint32_t **snapshot)
{
  if (*snapshot != NULL)
    {
      if (self->shadow != NULL && *snapshot == self->shadow->data)
        self->shadow_taken = FALSE;
      else
        g_free (*snapshot);
    }
  *snapshot = NULL;
}

/* Timer: once a big picture has not changed for a while, free the pixel
 * copies that are only kept for speed. The texture the screen is showing is
 * still held by GTK, so easel_syncbuf_trim leaves it alone. */
static gboolean
trim_tick (gpointer data)
{
  EaselCanvas *self = data;

  if (g_get_monotonic_time () - self->last_change_us < TRIM_IDLE_US)
    return G_SOURCE_CONTINUE;
  if (!self->shadow_taken)
    easel_syncbuf_trim (self->shadow);
  for (int i = 0; i < TEXTURE_BUFFERS; i++)
    easel_syncbuf_trim (self->tex_bufs[i]);
  self->trim_source = 0;
  return G_SOURCE_REMOVE;
}

/* Call this after every change to the pixels. It remembers the changed area
 * for the GPU upload (see build_texture), tells the pixel copies they are out
 * of date there, and schedules a redraw. */
void
canvas_mark_dirty (EaselCanvas *self, EaselRect r)
{
  if (r.width > 0 && r.height > 0)
    {
      self->change_count++;
      self->last_change_us = g_get_monotonic_time ();
      if (self->trim_source == 0 && (gsize) self->doc->width * self->doc->height * 4 >= TRIM_MIN_BYTES)
        self->trim_source = g_timeout_add_seconds (2, trim_tick, self);
      self->edit_rect = easel_rect_union (self->edit_rect, r);
      cairo_rectangle_int_t cr = { r.x, r.y, r.width, r.height };
      easel_syncbuf_mark_stale (self->shadow, r);
      for (int i = 0; i < TEXTURE_BUFFERS; i++)
        easel_syncbuf_mark_stale (self->tex_bufs[i], r);
      if (self->dirty == NULL)
        self->dirty = cairo_region_create_rectangle (&cr);
      else
        cairo_region_union_rectangle (self->dirty, &cr);
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

void
canvas_mark_all_dirty (EaselCanvas *self)
{
  canvas_mark_dirty (self, (EaselRect) { 0, 0, self->doc->width, self->doc->height });
}

/* Called by GDK when a texture is finished with its pixel memory. */
static void
release_texture_buffer (gpointer data)
{
  easel_syncbuf_unref (data);
}

/* Memory. While you draw, a picture needs the document, the undo shadow copy
 * and two texture copies (the one on screen and the one being built), so about
 * four times its size. Near the 2^28 pixel limit that is over 4 GB. To stay
 * out of the way afterwards, a picture of 64 MB or more gives the shadow and
 * the spare texture copy back after three idle seconds (see trim_tick), so it
 * rests at about twice its size. The first edit after that makes one full
 * copy again.
 *
 * The memory a new texture will read its pixels from. It can't be doc->pixels
 * itself: we keep drawing into that while GTK may still be rendering the
 * previous frame from it. So the texture gets a private copy. To keep that
 * cheap we recycle a few buffers and refresh only the changed rectangles
 * (see syncbuf.h). */
static GBytes *
texture_bytes (EaselCanvas *self)
{
  const EaselDocument *d = self->doc;
  gsize size = (gsize) d->width * d->height * 4;

  for (int i = 0; i < TEXTURE_BUFFERS; i++)
    {
      if (self->tex_bufs[i] == NULL)
        self->tex_bufs[i] = easel_syncbuf_new ();
      if (easel_syncbuf_busy (self->tex_bufs[i]))
        continue;

      EaselSyncBuf *b = self->tex_bufs[i];
      if (!easel_syncbuf_update (b, d))
        break;
      easel_syncbuf_ref (b);
      return g_bytes_new_with_free_func (b->data, size, release_texture_buffer, b);
    }
  /* All buffers are still held by GTK (very rare): fall back to a plain copy. */
  return g_bytes_new (d->pixels, size);
}

static GdkTexture *
build_texture (EaselCanvas *self)
{
  const EaselDocument *d = self->doc;
  gsize stride = (gsize) d->width * 4;
  g_autoptr (GBytes) bytes = texture_bytes (self);
  g_autoptr (GdkMemoryTextureBuilder) builder = gdk_memory_texture_builder_new ();

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  gdk_memory_texture_builder_set_format (builder, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED);
#else
  gdk_memory_texture_builder_set_format (builder, GDK_MEMORY_A8R8G8B8_PREMULTIPLIED);
#endif
  gdk_memory_texture_builder_set_width (builder, d->width);
  gdk_memory_texture_builder_set_height (builder, d->height);
  gdk_memory_texture_builder_set_stride (builder, stride);
  gdk_memory_texture_builder_set_bytes (builder, bytes);

  /* If we know the previous texture and which part of it is stale, tell GDK.
   * The GL/Vulkan renderer then uploads only that region instead of the
   * whole picture, which is what keeps drawing on a big canvas smooth. */
  if (self->texture != NULL && self->dirty != NULL
      && gdk_texture_get_width (self->texture) == d->width
      && gdk_texture_get_height (self->texture) == d->height)
    {
      gdk_memory_texture_builder_set_update_texture (builder, self->texture);
      gdk_memory_texture_builder_set_update_region (builder, self->dirty);
    }

  return gdk_memory_texture_builder_build (builder);
}

static void
refresh_texture (EaselCanvas *self)
{
  if (self->texture != NULL && self->dirty == NULL)
    return; /* no changes since the last upload */

  GdkTexture *fresh = build_texture (self);
  g_clear_object (&self->texture);
  self->texture = fresh;
  g_clear_pointer (&self->dirty, cairo_region_destroy);
}

static void
easel_canvas_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                      int *minimum, int *natural, int *minimum_baseline, int *natural_baseline)
{
  EaselCanvas *self = EASEL_CANVAS (widget);
  int pixels = orientation == GTK_ORIENTATION_HORIZONTAL ? self->doc->width : self->doc->height;
  int scale = gtk_widget_get_scale_factor (widget);
  int size = (int) ceil (pixels * self->zoom / 100.0 / scale) + HANDLE_PAD;

  *minimum = *natural = size;
  if (minimum_baseline != NULL)
    *minimum_baseline = -1;
  if (natural_baseline != NULL)
    *natural_baseline = -1;
}

/* Square under the pointer showing what the eraser will wipe. Black outline
 * with a white line inside, so you can see it on any background. */
static void
draw_eraser_outline (EaselCanvas *self, GtkSnapshot *snapshot, double s)
{
  if (self->tool != EASEL_TOOL_ERASER || self->hover_x < 0)
    return;

  const GdkRGBA black = { 0, 0, 0, 1 };
  const GdkRGBA white = { 1, 1, 1, 1 };
  const GdkRGBA black4[4] = { black, black, black, black };
  const GdkRGBA white4[4] = { white, white, white, white };
  const float width[4] = { 1, 1, 1, 1 };

  int size = self->tool_size;
  float left = (float) ((self->hover_x - size / 2) * s);
  float top = (float) ((self->hover_y - size / 2) * s);
  float side = (float) (size * s);
  graphene_rect_t outer_rect = GRAPHENE_RECT_INIT (left, top, side, side);
  graphene_rect_t inner_rect = GRAPHENE_RECT_INIT (left + 1, top + 1, side - 2, side - 2);
  GskRoundedRect outer, inner;

  gsk_rounded_rect_init_from_rect (&outer, &outer_rect, 0);
  gtk_snapshot_append_border (snapshot, &outer, width, black4);
  if (side > 4)
    {
      gsk_rounded_rect_init_from_rect (&inner, &inner_rect, 0);
      gtk_snapshot_append_border (snapshot, &inner, width, white4);
    }
}

/* Ghost of the next brush stamp under the pointer, in the colour the pressed
 * button would paint with (semi-transparent so you still see the picture).
 * It asks easel_brush_covers() pixel by pixel, the same function the real
 * stamp uses, so preview and result can never differ. */
static void
draw_brush_preview (EaselCanvas *self, GtkSnapshot *snapshot, double s)
{
  if ((self->tool != EASEL_TOOL_PENCIL && self->tool != EASEL_TOOL_BRUSH)
      || self->hover_x < 0 || self->drawing)
    return;

  EaselBrushShape shape = self->tool == EASEL_TOOL_BRUSH ? self->brush_shape : EASEL_BRUSH_CIRCLE;
  int size = self->tool_size;
  int left = self->hover_x - size / 2;
  int top = self->hover_y - size / 2;
  uint32_t c = self->foreground;
  GdkRGBA color = easel_argb_to_rgba (c, 0.65f);

  /* Merge pixels into horizontal runs. A big round brush would otherwise be
   * thousands of tiny rectangles for GSK to chew on. */
  for (int j = 0; j < size; j++)
    for (int i = 0; i < size;)
      {
        if (!easel_brush_covers (shape, size, i, j))
          {
            i++;
            continue;
          }
        int start = i;
        while (i < size && easel_brush_covers (shape, size, i, j))
          i++;
        graphene_rect_t run = GRAPHENE_RECT_INIT ((float) ((left + start) * s), (float) ((top + j) * s),
                                                  (float) ((i - start) * s), (float) s);
        gtk_snapshot_append_color (snapshot, &color, &run);
      }
}

/* Where handle number `handle` sits, in widget coordinates. */
static void
handle_center (EaselCanvas *self, int handle, double *x, double *y)
{
  double s = canvas_logical_per_pixel (self);
  double w = self->doc->width * s, h = self->doc->height * s;

  *x = handle == 2 ? w / 2 : w + HANDLE_SIZE / 2.0 + 1;
  *y = handle == 1 ? h / 2 : h + HANDLE_SIZE / 2.0 + 1;
}

static int
handle_at (EaselCanvas *self, double wx, double wy)
{
  for (int handle = 1; handle <= 3; handle++)
    {
      double cx, cy;
      handle_center (self, handle, &cx, &cy);
      if (fabs (wx - cx) <= 5 && fabs (wy - cy) <= 5)
        return handle;
    }
  return 0;
}

static void
draw_handles (EaselCanvas *self, GtkSnapshot *snapshot)
{
  const GdkRGBA dark = { 0.15f, 0.15f, 0.17f, 1 };
  const GdkRGBA light = { 1, 1, 1, 1 };

  for (int handle = 1; handle <= 3; handle++)
    {
      double cx, cy;
      handle_center (self, handle, &cx, &cy);
      graphene_rect_t halo = GRAPHENE_RECT_INIT ((float) (cx - HANDLE_SIZE / 2.0 - 1),
                                                 (float) (cy - HANDLE_SIZE / 2.0 - 1),
                                                 HANDLE_SIZE + 2, HANDLE_SIZE + 2);
      graphene_rect_t box = GRAPHENE_RECT_INIT ((float) (cx - HANDLE_SIZE / 2.0),
                                                (float) (cy - HANDLE_SIZE / 2.0),
                                                HANDLE_SIZE, HANDLE_SIZE);
      gtk_snapshot_append_color (snapshot, &light, &halo);
      gtk_snapshot_append_color (snapshot, &dark, &box);
    }

  /* While dragging, show where the new edge will be. */
  if (self->resize_handle != 0)
    {
      double s = canvas_logical_per_pixel (self);
      graphene_rect_t r = GRAPHENE_RECT_INIT (0, 0, (float) (self->resize_w * s),
                                              (float) (self->resize_h * s));
      GskRoundedRect rr;
      const GdkRGBA black4[4] = { dark, dark, dark, dark };
      const float width[4] = { 1, 1, 1, 1 };

      gsk_rounded_rect_init_from_rect (&rr, &r, 0);
      gtk_snapshot_append_border (snapshot, &rr, width, black4);
    }
}

/* The grid overlay, with lines one device pixel thick.
 *
 * Plain grid: a hard-edged repeating gradient is a line that repeats forever,
 * so the whole grid is two GSK nodes (vertical + horizontal) no matter how
 * big the picture is, instead of thousands of line segments.
 * Axonometric grid: those need diagonal lines, so we build a path.
 *
 * When cells get smaller than 4 screen pixels it would just be grey mush, so
 * nothing is drawn. */
static void
draw_grid (EaselCanvas *self, GtkSnapshot *snapshot, const graphene_rect_t *bounds, double s)
{
  if (!self->show_grid)
    return;

  GdkRGBA ink = { 0.45f, 0.45f, 0.5f, 0.55f }, none = { 0.45f, 0.45f, 0.5f, 0 };
  float line = 1.0f / (float) gtk_widget_get_scale_factor (GTK_WIDGET (self));

  if (self->grid_w * s >= 4)
    {
      float step = (float) (self->grid_w * s), cut = line / step;
      GskColorStop stops[4] = { { 0, ink }, { cut, ink }, { cut, none }, { 1, none } };
      graphene_point_t o = GRAPHENE_POINT_INIT (0, 0), right = GRAPHENE_POINT_INIT (step, 0);

      gtk_snapshot_append_repeating_linear_gradient (snapshot, bounds, &o, &right, stops, 4);
    }
  if (self->grid_h * s >= 4)
    {
      float step = (float) (self->grid_h * s), cut = line / step;
      GskColorStop stops[4] = { { 0, ink }, { cut, ink }, { cut, none }, { 1, none } };
      graphene_point_t o = GRAPHENE_POINT_INIT (0, 0), down = GRAPHENE_POINT_INIT (0, step);

      gtk_snapshot_append_repeating_linear_gradient (snapshot, bounds, &o, &down, stops, 4);
    }

  if (self->show_axo && self->axo_w * s >= 4)
    {
      float w = (float) (self->axo_w * s), pw = bounds->size.width, ph = bounds->size.height;
      int n = (int) ((pw + 2 * ph) / w) + 2;

      /* Lines slope 1:2, so each one is 2*ph wide and we start far enough to
       * the left to cover the corner. Skip if it would be an absurd number. */
      if (n < 4000)
        {
          GskPathBuilder *pb = gsk_path_builder_new ();

          for (int k = -(int) (2 * ph / w) - 1; k <= n; k++)
            {
              gsk_path_builder_move_to (pb, k * w, 0);          /* slope +1/2 */
              gsk_path_builder_line_to (pb, k * w + 2 * ph, ph);
            }
          for (int k = 0; k <= n + (int) (2 * ph / w) + 1; k++)
            {
              gsk_path_builder_move_to (pb, k * w, 0);          /* slope -1/2 */
              gsk_path_builder_line_to (pb, k * w - 2 * ph, ph);
            }
          GskPath *path = gsk_path_builder_free_to_path (pb);
          GskStroke *stroke = gsk_stroke_new (line);

          gtk_snapshot_push_clip (snapshot, bounds);
          gtk_snapshot_append_stroke (snapshot, path, stroke, &ink);
          gtk_snapshot_pop (snapshot);
          gsk_stroke_free (stroke);
          gsk_path_unref (path);
        }
    }
}

static void
easel_canvas_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  EaselCanvas *self = EASEL_CANVAS (widget);
  double s = canvas_logical_per_pixel (self);

  gint64 t0 = g_get_monotonic_time ();
  refresh_texture (self);

  graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0,
                                               (float) (self->doc->width * s),
                                               (float) (self->doc->height * s));
  /* Checkerboard under the picture so transparent areas are visible. It is
   * drawn as one 16x16 tile repeated, not as individual squares. */
  {
    graphene_rect_t tile = GRAPHENE_RECT_INIT (0, 0, 16, 16);
    graphene_rect_t a = GRAPHENE_RECT_INIT (0, 0, 8, 8), b = GRAPHENE_RECT_INIT (8, 8, 8, 8);
    const GdkRGBA light = { 1, 1, 1, 1 }, dark = { 0.82f, 0.82f, 0.82f, 1 };

    gtk_snapshot_push_repeat (snapshot, &bounds, &tile);
    gtk_snapshot_append_color (snapshot, &light, &tile);
    gtk_snapshot_append_color (snapshot, &dark, &a);
    gtk_snapshot_append_color (snapshot, &dark, &b);
    gtk_snapshot_pop (snapshot);
  }

  /* Thin edge and a soft shadow around the picture. The widget itself is a bit
   * bigger than the picture (room for the resize handles), so this follows the
   * picture's bounds, not the widget's. */
  {
    GskRoundedRect outline;
    const GdkRGBA edge = { 0, 0, 0, 0.30f };
    const GdkRGBA soft = { 0, 0, 0, 0.18f };

    gsk_rounded_rect_init_from_rect (&outline, &bounds, 0);
    gtk_snapshot_append_outset_shadow (snapshot, &outline, &soft, 0, 1, 0, 6);
    gtk_snapshot_append_outset_shadow (snapshot, &outline, &edge, 0, 0, 0.5f, 0);
  }

  /* Zoomed out we want smooth mip-mapped shrinking. At 100% and above, hard
   * square pixels, like the original Paint. */
  gtk_snapshot_append_scaled_texture (snapshot, self->texture,
                                      self->zoom < 100 ? GSK_SCALING_FILTER_TRILINEAR
                                                       : GSK_SCALING_FILTER_NEAREST, &bounds);

  /* Everything that sits on top of the picture, clipped to it. */
  gtk_snapshot_push_clip (snapshot, &bounds);
  draw_brush_preview (self, snapshot, s);
  draw_eraser_outline (self, snapshot, s);
  draw_grid (self, snapshot, &bounds, s);
  canvas_draw_selection (self, snapshot, s);
  canvas_draw_text (self, snapshot, s);
  canvas_draw_gradient_guide (self, snapshot, s);
  gtk_snapshot_pop (snapshot);

  draw_handles (self, snapshot);
  self->paint_us = g_get_monotonic_time () - t0;
}

static uint32_t
sample (EaselCanvas *self, int x, int y)
{
  /* Like old Paint: picking outside the picture gives white. */
  if (x < 0 || y < 0 || x >= self->doc->width || y >= self->doc->height)
    return EASEL_RGB (255, 255, 255);
  uint32_t c = easel_document_get_pixel (self->doc, x, y);
  if ((c >> 24) != 0xFF) /* see-through pixel: pick the colour you actually see, i.e. over white */
    {
      uint32_t miss = 255 - (c >> 24);
      return EASEL_RGB (MIN (((c >> 16) & 0xFF) + miss, 255u), MIN (((c >> 8) & 0xFF) + miss, 255u),
                        MIN ((c & 0xFF) + miss, 255u));
    }
  return c;
}

/* Stores the finished edit as one undo step. `base` is the picture from
 * before the edit; edit_rect says roughly where to look for differences. */
void
canvas_push_undo (EaselCanvas *self, const uint32_t *base)
{
  if (base != NULL && easel_history_push (self->history, self->doc, base, self->edit_rect))
    g_signal_emit (self, canvas_signals[SIGNAL_HISTORY_CHANGED], 0);
  self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
}

void
canvas_begin_edit (EaselCanvas *self)
{
  canvas_snapshot_drop (self, &self->edit_base);
  self->edit_base = canvas_snapshot_take (self);
  self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
}

void
canvas_finish_edit (EaselCanvas *self)
{
  if (self->edit_base == NULL)
    return;
  canvas_push_undo (self, self->edit_base);
  canvas_snapshot_drop (self, &self->edit_base);
}

/* ---- Mouse input ----
 *
 * One gesture is: press -> drag (many times) -> release. The three functions
 * below, canvas_press, canvas_drag and canvas_release, do what every tool
 * shares: the picture's resize handles, the right-click menus, panning, and
 * turning the pointer position into a picture pixel. Everything that depends
 * on the tool is handed to that tool's CanvasToolInput (see canvas-private.h):
 *   press   decides what the click means and sets up the state it needs
 *   drag    keeps updating that (extend the stroke, redraw the shape preview,
 *           move the selection, ...)
 *   release finishes it and commits it as a single undo step
 *
 * The handlers sit next to the code of their feature: text in canvas-text.c,
 * selections in canvas-selection.c, shapes, polygon, curve and gradient in
 * canvas-shapes.c. The simple ones are in this file.
 *
 * The three functions take plain numbers instead of a GTK gesture so the tests
 * can call them directly. The on_drag_* handlers further down only translate. */

/* Pan: drag the picture around inside the scrolled window. */
static void
pan_press (EaselCanvas *self, double x, double y, gboolean ctrl)
{
  GtkWidget *sw = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_SCROLLED_WINDOW);
  graphene_point_t in = GRAPHENE_POINT_INIT ((float) x, (float) y), out;

  if (sw == NULL || !gtk_widget_compute_point (GTK_WIDGET (self), sw, &in, &out))
    return;
  self->panning = TRUE;
  self->drawing = TRUE;
  self->pan_x0 = out.x;
  self->pan_y0 = out.y;
  self->pan_h0 = gtk_adjustment_get_value (gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (sw)));
  self->pan_v0 = gtk_adjustment_get_value (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (sw)));
  gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "grabbing");
}

static void
pan_drag (EaselCanvas *self, double offset_x, double offset_y)
{
  /* Measured against the scrolled window, which stays put while the
   * picture inside it scrolls. Using the canvas itself would make the
   * pan jitter. */
  GtkWidget *sw = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_SCROLLED_WINDOW);
  graphene_point_t in = GRAPHENE_POINT_INIT ((float) (self->start_x + offset_x), (float) (self->start_y + offset_y)), out;

  if (sw != NULL && gtk_widget_compute_point (GTK_WIDGET (self), sw, &in, &out))
    {
      gtk_adjustment_set_value (gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (sw)),
                                self->pan_h0 - (out.x - self->pan_x0));
      gtk_adjustment_set_value (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (sw)),
                                self->pan_v0 - (out.y - self->pan_y0));
    }
}

static void
magnifier_press (EaselCanvas *self, double x, double y, gboolean ctrl)
{
  easel_canvas_set_zoom (self, self->zoom == 100 ? self->zoom_level * 100 : 100,
                         self->last_x, self->last_y);
}

static void
fill_press (EaselCanvas *self, double x, double y, gboolean ctrl)
{
  canvas_begin_edit (self);
  canvas_mark_dirty (self, easel_raster_flood_fill (self->doc, self->last_x, self->last_y,
                                                    self->stroke_color));
  canvas_finish_edit (self);
}

static void
pick_press (EaselCanvas *self, double x, double y, gboolean ctrl)
{
  self->picked = sample (self, self->last_x, self->last_y);
  self->drawing = TRUE; /* resolved on release */
}

static void
pick_drag (EaselCanvas *self, int x, int y, gboolean shift)
{
  self->picked = sample (self, x, y);
}

static void
pick_release (EaselCanvas *self, int x, int y, gboolean shift)
{
  self->picked = sample (self, x, y);
  g_signal_emit (self, canvas_signals[SIGNAL_COLOR_PICKED], 0,
                 (guint) self->picked, self->button == GDK_BUTTON_PRIMARY);
}

static void
airbrush_press (EaselCanvas *self, double x, double y, gboolean ctrl)
{
  self->drawing = TRUE;
  canvas_begin_edit (self);
  canvas_spray_tick (self);
  g_clear_handle_id (&self->spray_source, g_source_remove);
  self->spray_source = g_timeout_add (EASEL_SPRAY_INTERVAL_MS, canvas_spray_tick, self);
}

static void
airbrush_release (EaselCanvas *self, int x, int y, gboolean shift)
{
  g_clear_handle_id (&self->spray_source, g_source_remove);
  canvas_finish_edit (self);
}

/* Pencil, brush and eraser: one stamp on press, then a line from the last
 * point to the new one on every drag. */
static void
freehand_press (EaselCanvas *self, double x, double y, gboolean ctrl)
{
  self->drawing = TRUE;
  canvas_begin_edit (self);
  canvas_mark_dirty (self, canvas_apply_stroke (self, self->last_x, self->last_y,
                                                self->last_x, self->last_y));
}

static void
freehand_drag (EaselCanvas *self, int x, int y, gboolean shift)
{
  /* The mouse only reports now and then, so a fast stroke would be dotted.
   * Join the last point to this one with a line. */
  canvas_mark_dirty (self, canvas_apply_stroke (self, self->last_x, self->last_y, x, y));
}

static void
freehand_release (EaselCanvas *self, int x, int y, gboolean shift)
{
  canvas_finish_edit (self);
}

static const CanvasToolInput input_pan = { pan_press, NULL, NULL };
static const CanvasToolInput input_magnifier = { magnifier_press, NULL, NULL };
static const CanvasToolInput input_fill = { fill_press, NULL, NULL };
static const CanvasToolInput input_pick = { pick_press, pick_drag, pick_release };
static const CanvasToolInput input_airbrush = { airbrush_press, NULL, airbrush_release };
static const CanvasToolInput input_freehand = { freehand_press, freehand_drag, freehand_release };

static const CanvasToolInput *
input_for_tool (EaselTool tool)
{
  switch (tool)
    {
    case EASEL_TOOL_PAN:                return &input_pan;
    case EASEL_TOOL_MAGNIFIER:          return &input_magnifier;
    case EASEL_TOOL_FILL:               return &input_fill;
    case EASEL_TOOL_PICK_COLOR:         return &input_pick;
    case EASEL_TOOL_AIRBRUSH:           return &input_airbrush;
    case EASEL_TOOL_PENCIL:
    case EASEL_TOOL_BRUSH:
    case EASEL_TOOL_ERASER:             return &input_freehand;
    case EASEL_TOOL_TEXT:               return &canvas_input_text;
    case EASEL_TOOL_SELECT:
    case EASEL_TOOL_FREE_FORM_SELECT:   return &canvas_input_selection;
    case EASEL_TOOL_GRADIENT:           return &canvas_input_gradient;
    case EASEL_TOOL_LINE:
    case EASEL_TOOL_RECTANGLE:
    case EASEL_TOOL_ELLIPSE:
    case EASEL_TOOL_ROUNDED_RECTANGLE:  return &canvas_input_shape;
    case EASEL_TOOL_POLYGON:            return &canvas_input_polygon;
    case EASEL_TOOL_CURVE:              return &canvas_input_curve;
    default:                            return NULL;
    }
}

/* A press on one of the picture's resize handles starts a resize, whatever
 * the tool. Returns TRUE if it did. */
static gboolean
resize_press (EaselCanvas *self, double x, double y)
{
  int handle = handle_at (self, x, y);

  if (handle == 0)
    return FALSE;
  canvas_commit_seq (self, FALSE);
  self->resize_handle = handle;
  if (self->handle_hover != handle)
    {
      static const char *const names[] = { NULL, "ew-resize", "ns-resize", "nwse-resize" };
      self->handle_hover = handle;
      gtk_widget_set_cursor_from_name (GTK_WIDGET (self), names[handle]);
    }
  self->resize_w = self->doc->width;
  self->resize_h = self->doc->height;
  return TRUE;
}

static void
resize_drag (EaselCanvas *self, double offset_x, double offset_y)
{
  double s = canvas_logical_per_pixel (self);
  int nx = (int) lround ((self->start_x + offset_x) / s);
  int ny = (int) lround ((self->start_y + offset_y) / s);

  /* The handle sits a little outside the picture, so work out the new size
   * from how far the pointer moved since the press, not from its absolute
   * position. */
  nx = self->doc->width + (nx - (int) lround (self->start_x / s));
  ny = self->doc->height + (ny - (int) lround (self->start_y / s));
  if (self->resize_handle != 2)
    self->resize_w = CLAMP (nx, 1, EASEL_MAX_DIMENSION);
  if (self->resize_handle != 1)
    self->resize_h = CLAMP (ny, 1, EASEL_MAX_DIMENSION);
  g_signal_emit (self, canvas_signals[SIGNAL_RESIZE_PREVIEW], 0, self->resize_w, self->resize_h);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
resize_release (EaselCanvas *self)
{
  self->resize_handle = 0;
  self->handle_hover = -1; /* forces the next mouse move to pick the right cursor again */
  gtk_widget_set_cursor (GTK_WIDGET (self), easel_cursor_for_tool (self->tool));
  g_signal_emit (self, canvas_signals[SIGNAL_RESIZE_PREVIEW], 0, -1, -1);
  if (self->resize_w != self->doc->width || self->resize_h != self->doc->height)
    easel_canvas_resize (self, self->resize_w, self->resize_h);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* Right button on a selection or text box doesn't draw, it opens a context
 * menu. It is opened on release (see ctx_pending), so the click that asks
 * for it isn't also delivered to the popup. Returns TRUE if one is pending. */
static gboolean
context_menu_press (EaselCanvas *self, double x, double y)
{
  int ix, iy, kind = -1;

  to_image (self, x, y, &ix, &iy);
  if ((easel_tool_info (self->tool)->flags & EASEL_TOOL_SELECTION) && canvas_sel_contains (self, ix, iy))
    kind = 0;
  else if (self->tool == EASEL_TOOL_TEXT && self->text_active
           && canvas_text_hit (self, x, y) != TEXT_HIT_NONE)
    kind = 1;
  else if (self->tool == EASEL_TOOL_PAN || (easel_tool_info (self->tool)->flags & EASEL_TOOL_SELECTION))
    kind = 2; /* Pan, or a selection tool outside the selection: Paste and friends */
  if (kind < 0 || self->ctx_model[kind] == NULL)
    return FALSE;
  self->ctx_pending = kind;
  self->ctx_x = x;
  self->ctx_y = y;
  return TRUE;
}

static void
context_menu_release (EaselCanvas *self)
{
  int kind = self->ctx_pending;
  GdkRectangle at = { (int) self->ctx_x, (int) self->ctx_y, 1, 1 };

  self->ctx_pending = -1;
  if (self->ctx_popover[kind] == NULL)
    {
      self->ctx_popover[kind] = gtk_popover_menu_new_from_model (self->ctx_model[kind]);
      gtk_widget_set_parent (self->ctx_popover[kind], GTK_WIDGET (self));
      gtk_popover_set_has_arrow (GTK_POPOVER (self->ctx_popover[kind]), FALSE);
      gtk_widget_set_halign (self->ctx_popover[kind], GTK_ALIGN_START);
    }
  gtk_popover_set_pointing_to (GTK_POPOVER (self->ctx_popover[kind]), &at);
  gtk_popover_popup (GTK_POPOVER (self->ctx_popover[kind]));
}

void
canvas_press (EaselCanvas *self, guint button, double x, double y, gboolean ctrl)
{
  if (button == GDK_BUTTON_PRIMARY && resize_press (self, x, y))
    return;
  if (button == GDK_BUTTON_SECONDARY && context_menu_press (self, x, y))
    return;

  /* Same as the original: left button draws in the foreground colour, right
   * button in the background colour. */
  self->button = button;
  self->stroke_color = button == GDK_BUTTON_PRIMARY ? self->foreground : self->background;
  self->start_x = x;
  self->start_y = y;
  to_image (self, x, y, &self->last_x, &self->last_y);
  self->start_ix = self->last_x;
  self->start_iy = self->last_y;

  const CanvasToolInput *input = input_for_tool (self->tool);

  if (input != NULL && input->press != NULL)
    input->press (self, x, y, ctrl);
}

void
canvas_drag (EaselCanvas *self, double offset_x, double offset_y, gboolean shift)
{
  int x, y;

  if (self->resize_handle != 0)
    {
      resize_drag (self, offset_x, offset_y);
      return;
    }
  if (!self->drawing)
    return;
  if (self->panning)
    {
      pan_drag (self, offset_x, offset_y);
      return;
    }

  to_image (self, self->start_x + offset_x, self->start_y + offset_y, &x, &y);
  if (self->grad_drag != 0)
    {
      canvas_grad_drag_to (self, x, y);
      return;
    }
  if (x == self->last_x && y == self->last_y)
    return;

  const CanvasToolInput *input = input_for_tool (self->tool);

  if (input != NULL && input->drag != NULL)
    input->drag (self, x, y, shift);
  self->last_x = x;
  self->last_y = y;
}

void
canvas_release (EaselCanvas *self, double offset_x, double offset_y, gboolean shift)
{
  int x, y;

  if (self->ctx_pending >= 0)
    {
      context_menu_release (self);
      return;
    }
  if (self->resize_handle != 0)
    {
      resize_release (self);
      return;
    }
  if (!self->drawing)
    return;
  self->drawing = FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self)); /* bring the brush preview back */

  if (self->panning)
    {
      self->panning = FALSE;
      gtk_widget_set_cursor (GTK_WIDGET (self), easel_cursor_for_tool (self->tool));
      return;
    }

  to_image (self, self->start_x + offset_x, self->start_y + offset_y, &x, &y);

  const CanvasToolInput *input = input_for_tool (self->tool);

  if (input != NULL && input->release != NULL)
    input->release (self, x, y, shift);
}

static gboolean
modifier_held (GtkGestureDrag *gesture, GdkModifierType mask)
{
  GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));

  return (state & mask) != 0;
}

static void
on_drag_begin (GtkGestureDrag *gesture, double x, double y, EaselCanvas *self)
{
  guint button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));

  if (button != GDK_BUTTON_PRIMARY && button != GDK_BUTTON_SECONDARY)
    {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }
  canvas_press (self, button, x, y, modifier_held (gesture, GDK_CONTROL_MASK));
}

static void
on_drag_update (GtkGestureDrag *gesture, double offset_x, double offset_y, EaselCanvas *self)
{
  canvas_drag (self, offset_x, offset_y, modifier_held (gesture, GDK_SHIFT_MASK));
}

static void
on_drag_end (GtkGestureDrag *gesture, double offset_x, double offset_y, EaselCanvas *self)
{
  canvas_release (self, offset_x, offset_y, modifier_held (gesture, GDK_SHIFT_MASK));
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller, guint keyval, guint keycode,
                GdkModifierType state, EaselCanvas *self)
{
  if (self->text_active && canvas_text_key (self, keyval, state))
    return TRUE;

  if (keyval == GDK_KEY_Delete && self->sel_active)
    {
      easel_canvas_clear_selection (self);
      return TRUE;
    }

  if (self->grad_pending && (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter))
    {
      canvas_grad_commit (self);
      return TRUE;
    }
  if (self->grad_pending && keyval == GDK_KEY_Escape)
    {
      canvas_end_shape_drag (self, FALSE);
      self->grad_pending = FALSE;
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return TRUE;
    }

  if (keyval != GDK_KEY_Escape)
    return FALSE;

  if (self->sel_active || self->sel_creating)
    canvas_sel_commit (self);
  else if (self->seq_active)
    canvas_cancel_seq (self);
  else if (self->drawing && (easel_tool_info (self->tool)->flags & EASEL_TOOL_SHAPE))
    {
      canvas_end_shape_drag (self, FALSE);
      self->drawing = FALSE;
    }
  else
    return FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  return TRUE;
}

static void
on_motion (GtkEventControllerMotion *controller, double x, double y, EaselCanvas *self)
{
  /* Pointer over a selection: show the "move" cursor. */
  if ((easel_tool_info (self->tool)->flags & EASEL_TOOL_SELECTION) && self->resize_handle == 0 && self->handle_hover == 0)
    {
      int ix, iy;
      static const char *const stretch[] = { NULL, "nwse-resize", "ns-resize", "nesw-resize", "ew-resize",
                                             "ew-resize", "nesw-resize", "ns-resize", "nwse-resize" };
      int sh = self->sel_stretch != 0 ? self->sel_stretch : canvas_sel_handle_at (self, x, y);
      to_image (self, x, y, &ix, &iy);
      if (sh != 0)
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self), stretch[sh]);
      else if (canvas_sel_contains (self, ix, iy))
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "move");
      else
        gtk_widget_set_cursor (GTK_WIDGET (self), easel_cursor_for_tool (self->tool));
    }

  if (self->tool == EASEL_TOOL_TEXT && self->resize_handle == 0 && self->handle_hover == 0)
    {
      static const char *const grips[] = { NULL, "nwse-resize", "ns-resize", "nesw-resize", "ew-resize",
                                           "ew-resize", "nesw-resize", "ns-resize", "nwse-resize" };
      int hit = self->text_drag == TEXT_DRAG_MOVE ? TEXT_HIT_BORDER
                : self->text_drag == TEXT_DRAG_RESIZE ? self->text_handle
                : self->text_drag == TEXT_DRAG_SELECT ? TEXT_HIT_INSIDE : canvas_text_hit (self, x, y);

      if (hit >= 1 && hit <= 8)
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self), grips[hit]);
      else if (hit == TEXT_HIT_BORDER)
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "move");
      else if (hit == TEXT_HIT_INSIDE)
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "text");
      else
        gtk_widget_set_cursor (GTK_WIDGET (self), easel_cursor_for_tool (self->tool));
    }

  /* Keep the resize cursor while a handle is being dragged, even if the
   * pointer wanders off it. */
  int handle = self->resize_handle != 0 ? self->resize_handle : handle_at (self, x, y);
  if (handle != self->handle_hover)
    {
      static const char *const names[] = { NULL, "ew-resize", "ns-resize", "nwse-resize" };

      self->handle_hover = handle;
      if (handle != 0)
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self), names[handle]);
      else
        gtk_widget_set_cursor (GTK_WIDGET (self), easel_cursor_for_tool (self->tool));
    }
  to_image (self, x, y, &self->hover_x, &self->hover_y);
  g_signal_emit (self, canvas_signals[SIGNAL_POINTER_MOVED], 0, self->hover_x, self->hover_y);
  if (self->seq_active && !self->drawing && self->tool == EASEL_TOOL_POLYGON)
    canvas_seq_render (self, FALSE, TRUE, self->hover_x, self->hover_y);
  if ((easel_tool_info (self->tool)->flags & EASEL_TOOL_HOVER_PREVIEW))
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_leave (GtkEventControllerMotion *controller, EaselCanvas *self)
{
  self->hover_x = self->hover_y = -1;
  g_signal_emit (self, canvas_signals[SIGNAL_POINTER_MOVED], 0, -1, -1);
  if ((easel_tool_info (self->tool)->flags & EASEL_TOOL_HOVER_PREVIEW))
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_scale_factor_changed (EaselCanvas *self)
{
  gtk_widget_queue_resize (GTK_WIDGET (self));
}

static void
easel_canvas_dispose (GObject *object)
{
  EaselCanvas *self = EASEL_CANVAS (object);
  for (int i = 0; i < 3; i++)
    {
      g_clear_pointer (&self->ctx_popover[i], gtk_widget_unparent);
      g_clear_object (&self->ctx_model[i]);
    }
  g_clear_object (&self->texture);
  g_clear_pointer (&self->dirty, cairo_region_destroy);
  canvas_snapshot_drop (self, &self->backup);
  g_clear_handle_id (&self->spray_source, g_source_remove);
  g_clear_handle_id (&self->trim_source, g_source_remove);
  g_clear_pointer (&self->seq, g_free);
  canvas_snapshot_drop (self, &self->edit_base);
  g_clear_pointer (&self->history, easel_history_free);
  g_clear_pointer (&self->shadow, easel_syncbuf_unref);
  for (int i = 0; i < TEXTURE_BUFFERS; i++)
    g_clear_pointer (&self->tex_bufs[i], easel_syncbuf_unref);
  g_clear_pointer (&self->sel_clip, easel_clip_free);
  canvas_snapshot_drop (self, &self->sel_base);
  g_clear_object (&self->sel_texture);
  canvas_text_free_render (self);
  if (self->text_buf != NULL)
    g_string_free (self->text_buf, TRUE);
  self->text_buf = NULL;
  if (self->text_attr != NULL)
    g_array_unref (self->text_attr);
  self->text_attr = NULL;
  g_clear_pointer (&self->font, g_free);
  G_OBJECT_CLASS (easel_canvas_parent_class)->dispose (object);
}

static void
easel_canvas_finalize (GObject *object)
{
  EaselCanvas *self = EASEL_CANVAS (object);
  easel_document_free (self->doc);
  self->doc = NULL;
  G_OBJECT_CLASS (easel_canvas_parent_class)->finalize (object);
}

static void
easel_canvas_class_init (EaselCanvasClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = easel_canvas_dispose;
  object_class->finalize = easel_canvas_finalize;
  widget_class->measure = easel_canvas_measure;
  widget_class->snapshot = easel_canvas_snapshot;

  canvas_signals[SIGNAL_POINTER_MOVED] =
    g_signal_new ("pointer-moved", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
  canvas_signals[SIGNAL_HISTORY_CHANGED] =
    g_signal_new ("history-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  canvas_signals[SIGNAL_SELECTION_CHANGED] =
    g_signal_new ("selection-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  canvas_signals[SIGNAL_TEXT_STATE] =
    g_signal_new ("text-state", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
  canvas_signals[SIGNAL_ZOOM_CHANGED] =
    g_signal_new ("zoom-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  canvas_signals[SIGNAL_RESIZE_PREVIEW] =
    g_signal_new ("resize-preview", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
  canvas_signals[SIGNAL_COLOR_PICKED] =
    g_signal_new ("color-picked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_BOOLEAN);
}

/* The canvas owns the popovers, the window supplies the models and the actions
 * they trigger. Called once at start-up. */
void
easel_canvas_set_context_menus (EaselCanvas *self, GMenuModel *selection, GMenuModel *text, GMenuModel *pan)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  GMenuModel *models[3] = { selection, text, pan };

  for (int i = 0; i < 3; i++)
    {
      g_clear_pointer (&self->ctx_popover[i], gtk_widget_unparent);
      g_set_object (&self->ctx_model[i], models[i]);
    }
}

/* One zoom step in (+1) or out (-1), keeping picture pixel (ix, iy) where it
 * is on screen. Pass a negative ix to zoom around the middle of the window. */
void
easel_canvas_zoom_step (EaselCanvas *self, int direction, int ix, int iy)
{
  static const int steps[] = EASEL_ZOOM_STEPS;
  int n = G_N_ELEMENTS (steps), i = 0;

  while (i < n - 1 && steps[i] < self->zoom)
    i++;
  i = CLAMP (i + direction, 0, n - 1);
  easel_canvas_set_zoom (self, steps[i], ix >= 0 ? ix : -1, iy >= 0 ? iy : -1);
}

static gboolean
on_scroll (GtkEventControllerScroll *controller, double dx, double dy, EaselCanvas *self)
{
  GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (controller));
  if (!(state & GDK_CONTROL_MASK) || dy == 0)
    return GDK_EVENT_PROPAGATE;

  easel_canvas_zoom_step (self, dy < 0 ? 1 : -1, self->hover_x, self->hover_y);
  return GDK_EVENT_STOP;
}

static void
easel_canvas_init (EaselCanvas *self)
{
  self->font = g_strdup (EASEL_DEFAULT_FONT);
  self->text_anchor = -1;
  self->zoom = 100;
  self->foreground = EASEL_RGB (0, 0, 0);
  self->background = EASEL_RGB (255, 255, 255);
  self->tool = EASEL_TOOL_PAN;
  self->tool_size = 1;
  self->brush_shape = EASEL_BRUSH_CIRCLE;
  self->line_width = 1;
  self->zoom_level = 2;
  self->sel_opaque = TRUE;
  self->eraser_opaque = TRUE;
  self->ctx_pending = -1;
  self->grid_w = self->grid_h = 1;
  self->axo_w = 64;
  self->history = easel_history_new ((size_t) CLAMP (easel_pref_int ("undo-limit-mb", EASEL_UNDO_LIMIT_MB), 16, 32768) << 20);
  {
    /* Old undo steps go to a temporary file in the cache folder. */
    char *dir = g_build_filename (g_get_user_cache_dir (), "easel", NULL);

    if (g_mkdir_with_parents (dir, 0700) != 0)
      g_clear_pointer (&dir, g_free); /* fall back to $TMPDIR */
    easel_history_set_spill (self->history, dir, (size_t) EASEL_UNDO_RAM_MB << 20);
    g_free (dir);
  }
  self->spray_rng = 2463534242u;
  self->fill_style = EASEL_FILL_OUTLINE;
  self->hover_x = self->hover_y = -1;

  gtk_widget_add_css_class (GTK_WIDGET (self), "easel-canvas");
  gtk_widget_set_cursor (GTK_WIDGET (self), easel_cursor_for_tool (self->tool));

  GtkGesture *drag = gtk_gesture_drag_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), 0); /* any button */
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));

  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);

  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
  g_signal_connect (motion, "leave", G_CALLBACK (on_leave), self);
  gtk_widget_add_controller (GTK_WIDGET (self), motion);

  GtkEventController *wheel = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  gtk_event_controller_set_propagation_phase (wheel, GTK_PHASE_CAPTURE);
  g_signal_connect (wheel, "scroll", G_CALLBACK (on_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self), wheel);

  g_signal_connect (self, "notify::scale-factor", G_CALLBACK (on_scale_factor_changed), NULL);
}

GtkWidget *
easel_canvas_new (EaselDocument *doc)
{
  g_return_val_if_fail (doc != NULL, NULL);

  EaselCanvas *self = g_object_new (EASEL_TYPE_CANVAS, NULL);
  self->doc = doc;
  return GTK_WIDGET (self);
}

EaselDocument *
easel_canvas_get_document (EaselCanvas *self)
{
  g_return_val_if_fail (EASEL_IS_CANVAS (self), NULL);
  return self->doc;
}

void
easel_canvas_set_foreground (EaselCanvas *self, uint32_t argb)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->foreground = argb;
  if (self->grad_pending)
    canvas_grad_render (self);
  canvas_text_apply_color (self, argb);
}

void
easel_canvas_set_background (EaselCanvas *self, uint32_t argb)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->background = argb;
  if (self->grad_pending)
    canvas_grad_render (self);
  canvas_text_render (self);
  if (self->sel_clip != NULL && !self->sel_opaque)
    canvas_sel_refresh_texture (self);
}

void
easel_canvas_set_tool (EaselCanvas *self, EaselTool tool)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (tool != self->tool)
    {
      canvas_sel_commit (self);
      canvas_commit_seq (self, FALSE);
    }
  self->tool = tool;
  if (self->handle_hover == 0)
    gtk_widget_set_cursor (GTK_WIDGET (self), easel_cursor_for_tool (tool));
  gtk_widget_queue_draw (GTK_WIDGET (self)); /* hover overlay appears or goes away */
}

void
easel_canvas_set_tool_size (EaselCanvas *self, int size)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->tool_size = CLAMP (size, 1, EASEL_MAX_BRUSH);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
easel_canvas_set_brush_shape (EaselCanvas *self, EaselBrushShape shape)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->brush_shape = shape;
}

void
easel_canvas_invalidate (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (self->seq_active)
    canvas_end_seq (self); /* the picture changed under it */
  canvas_mark_all_dirty (self);
}

void
easel_canvas_set_line_width (EaselCanvas *self, int width)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->line_width = CLAMP (width, 1, EASEL_MAX_BRUSH);
}

void
easel_canvas_set_fill_style (EaselCanvas *self, EaselFillStyle style)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->fill_style = style;
}

static void
apply_center (gpointer data)
{
  EaselCanvas *self = data;
  GtkWidget *sw = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_SCROLLED_WINDOW);

  if (sw != NULL && self->center_pending)
    {
      double s = canvas_logical_per_pixel (self);
      GtkAdjustment *h = gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (sw));
      GtkAdjustment *v = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (sw));

      gtk_adjustment_set_value (h, self->center_x * s - gtk_adjustment_get_page_size (h) / 2);
      gtk_adjustment_set_value (v, self->center_y * s - gtk_adjustment_get_page_size (v) / 2);
    }
  self->center_pending = FALSE;
  g_object_unref (self);
}

/* Sets the zoom and scrolls so picture point (ix, iy) ends up in the middle
 * of the view; ix < 0 means "whatever is in the middle now". The scrolling
 * has to wait until GTK has resized the canvas for the new zoom, which is why
 * it's done later from `apply_center`. */
void
easel_canvas_set_zoom (EaselCanvas *self, int zoom, int ix, int iy)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  zoom = CLAMP (zoom, EASEL_MIN_ZOOM, EASEL_MAX_ZOOM);
  if (zoom == self->zoom)
    return;

  GtkWidget *sw = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_SCROLLED_WINDOW);
  if (ix < 0 && sw != NULL)
    {
      double s = canvas_logical_per_pixel (self);
      GtkAdjustment *h = gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (sw));
      GtkAdjustment *v = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (sw));
      ix = (int) ((gtk_adjustment_get_value (h) + gtk_adjustment_get_page_size (h) / 2) / s);
      iy = (int) ((gtk_adjustment_get_value (v) + gtk_adjustment_get_page_size (v) / 2) / s);
      ix = CLAMP (ix, 0, self->doc->width - 1);
      iy = CLAMP (iy, 0, self->doc->height - 1);
    }

  self->zoom = zoom;
  if (self->sel_clip != NULL && !self->sel_opaque)
    canvas_sel_refresh_texture (self); /* the checkerboard inside a selection depends on the zoom */
  g_signal_emit (self, canvas_signals[SIGNAL_ZOOM_CHANGED], 0);
  self->center_x = ix + 0.5;
  self->center_y = iy + 0.5;
  gtk_widget_queue_resize (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));

  if (!self->center_pending)
    {
      self->center_pending = TRUE;
      g_idle_add_once (apply_center, g_object_ref (self));
    }
}

guint64
easel_canvas_get_change_count (EaselCanvas *self)
{
  return self->change_count;
}

gint64
easel_canvas_get_paint_time (EaselCanvas *self)
{
  return self->paint_us;
}

int
easel_canvas_get_zoom (EaselCanvas *self)
{
  g_return_val_if_fail (EASEL_IS_CANVAS (self), 1);
  return self->zoom;
}

void
easel_canvas_set_zoom_level (EaselCanvas *self, int level)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->zoom_level = CLAMP (level, 1, 8);
}

void
easel_canvas_mark_saved (EaselCanvas *self)
{
  easel_history_mark_saved (self->history);
}

gboolean
easel_canvas_is_saved (EaselCanvas *self)
{
  return easel_history_is_saved (self->history);
}

gboolean
easel_canvas_can_undo (EaselCanvas *self)
{
  g_return_val_if_fail (EASEL_IS_CANVAS (self), FALSE);
  return easel_history_can_undo (self->history);
}

gboolean
easel_canvas_can_redo (EaselCanvas *self)
{
  g_return_val_if_fail (EASEL_IS_CANVAS (self), FALSE);
  return easel_history_can_redo (self->history);
}

gboolean
easel_canvas_undo (EaselCanvas *self)
{
  g_return_val_if_fail (EASEL_IS_CANVAS (self), FALSE);
  EaselRect r;

  canvas_sel_commit (self);
  if (self->seq_active) /* Ctrl+Z while a polygon/curve is half done just cancels it */
    {
      canvas_cancel_seq (self);
      return TRUE;
    }
  if (self->drawing || !easel_history_undo (self->history, self->doc, &r))
    return FALSE;
  gtk_widget_queue_resize (GTK_WIDGET (self)); /* the undone step may have changed the size */
  canvas_mark_all_dirty (self);
  self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
  g_signal_emit (self, canvas_signals[SIGNAL_HISTORY_CHANGED], 0);
  return TRUE;
}

gboolean
easel_canvas_redo (EaselCanvas *self)
{
  g_return_val_if_fail (EASEL_IS_CANVAS (self), FALSE);
  EaselRect r;

  canvas_sel_commit (self);
  if (self->drawing || self->seq_active || !easel_history_redo (self->history, self->doc, &r))
    return FALSE;
  gtk_widget_queue_resize (GTK_WIDGET (self)); /* the undone step may have changed the size */
  canvas_mark_all_dirty (self);
  self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
  g_signal_emit (self, canvas_signals[SIGNAL_HISTORY_CHANGED], 0);
  return TRUE;
}

/* Image > Clear Image. One undo step. */
void
easel_canvas_clear (EaselCanvas *self, uint32_t argb)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  canvas_sel_commit (self);
  canvas_commit_seq (self, FALSE);
  canvas_begin_edit (self);
  easel_document_fill (self->doc, argb);
  canvas_mark_all_dirty (self);
  canvas_finish_edit (self);
}

/* Swaps in a different picture (File > New / Open). Takes ownership of `doc`.
 * Undo history and any half-finished tool work are thrown away. */
void
easel_canvas_set_document (EaselCanvas *self, EaselDocument *doc)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  g_return_if_fail (doc != NULL);

  g_clear_handle_id (&self->spray_source, g_source_remove);
  canvas_sel_discard (self);
  canvas_sel_notify (self);
  canvas_snapshot_drop (self, &self->backup);
  canvas_snapshot_drop (self, &self->edit_base);
  self->grad_pending = FALSE;
  self->grad_drag = 0;
  self->seq_active = FALSE;
  self->seq_stage = 0;
  self->seq_n = 0;
  self->drawing = FALSE;
  self->shape_prev = self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
  easel_history_clear (self->history);

  easel_document_free (self->doc);
  self->doc = doc;
  g_clear_object (&self->texture);
  g_clear_pointer (&self->dirty, cairo_region_destroy);
  self->zoom = 100;
  g_signal_emit (self, canvas_signals[SIGNAL_ZOOM_CHANGED], 0);
  gtk_widget_queue_resize (GTK_WIDGET (self));
  canvas_mark_all_dirty (self);
  g_signal_emit (self, canvas_signals[SIGNAL_HISTORY_CHANGED], 0);
}

/* Makes the picture bigger (new area in the background colour) or smaller
 * (crops). One undo step. */
gboolean
easel_canvas_resize (EaselCanvas *self, int width, int height)
{
  g_return_val_if_fail (EASEL_IS_CANVAS (self), FALSE);

  canvas_sel_commit (self);
  if (width == self->doc->width && height == self->doc->height)
    return TRUE;
  canvas_commit_seq (self, FALSE);

  int old_w = self->doc->width, old_h = self->doc->height;
  g_autofree uint32_t *old = g_memdup2 (self->doc->pixels, (gsize) old_w * old_h * 4);

  if (!easel_document_resize (self->doc, width, height, self->background))
    return FALSE;

  easel_history_push_full (self->history, old, old_w, old_h, self->doc);
  gtk_widget_queue_resize (GTK_WIDGET (self));
  canvas_mark_all_dirty (self);
  g_signal_emit (self, canvas_signals[SIGNAL_HISTORY_CHANGED], 0);
  return TRUE;
}

void
easel_canvas_set_grid (EaselCanvas *self, gboolean show, int grid_w, int grid_h, gboolean axo, int axo_w)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->show_grid = show;
  self->grid_w = CLAMP (grid_w, 1, 4096);
  self->grid_h = CLAMP (grid_h, 1, 4096);
  self->show_axo = axo;
  self->axo_w = CLAMP (axo_w, 1, 4096);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
easel_canvas_get_grid (EaselCanvas *self, gboolean *show, int *grid_w, int *grid_h, gboolean *axo, int *axo_w)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  *show = self->show_grid;
  *grid_w = self->grid_w;
  *grid_h = self->grid_h;
  *axo = self->show_axo;
  *axo_w = self->axo_w;
}

/* Zoom so the whole picture just fits in the window. Leaves the same strip
 * of room on the right and bottom that is already kept for the resize handles. */
void
easel_canvas_zoom_fit (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  GtkWidget *sw = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_SCROLLED_WINDOW);

  if (sw == NULL)
    return;

  double pw = gtk_adjustment_get_page_size (gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (sw)));
  double ph = gtk_adjustment_get_page_size (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (sw)));
  double kx = (pw - 2 * HANDLE_PAD) / self->doc->width, ky = (ph - 2 * HANDLE_PAD) / self->doc->height;

  easel_canvas_set_zoom (self, (int) (MIN (kx, ky) * 100), self->doc->width / 2, self->doc->height / 2);
}

