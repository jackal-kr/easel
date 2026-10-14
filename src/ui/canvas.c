#include "ui/canvas-private.h"

G_DEFINE_FINAL_TYPE (EaselCanvas, easel_canvas, GTK_TYPE_WIDGET)

guint canvas_signals[N_SIGNALS];

/* One image pixel is one *device* pixel at zoom 1, even on HiDPI screens,
 * so the logical widget size is image size * zoom / scale factor. */
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

/* The picture as it is right now, as a read-only "before" image for undo and
 * live shape previews. It costs no copy of the whole picture after the first
 * time, only of the rectangles drawn since. If a previous holder still has it,
 * a private full copy is made instead. Give it back with canvas_snapshot_drop. */
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

/* Records which image pixels changed so that only that region is re-uploaded
 * to the GPU (see build_texture). */
void
canvas_mark_dirty (EaselCanvas *self, EaselRect r)
{
  if (r.width > 0 && r.height > 0)
    {
      self->change_count++;
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

/* The texture holds a private copy of the pixels (the document keeps changing
 * while GTK may still be rendering an older frame). Passing the previous
 * texture and the changed region lets GSK upload just that region to the GPU
 * instead of the whole image. */
static void
release_texture_buffer (gpointer data)
{
  easel_syncbuf_unref (data);
}

/* Hands the texture its own copy of the pixels (the picture keeps changing
 * while GTK may still be drawing an older frame). Copies are recycled and
 * brought up to date by copying only the rectangles that changed. */
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
  return g_bytes_new (d->pixels, size); /* every buffer is still in use: plain copy */
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
    return; /* nothing changed since the last upload */

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

/* The eraser shows its square under the pointer: black outline with a white
 * line inside it, so it is visible on any colour. */
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

/* Shows the exact pixels the next pencil/brush stamp would paint, in the
 * colour the pressed button would use (translucent so the picture stays
 * readable). Uses the engine's own footprint test, so it cannot disagree. */
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

  /* One rectangle per horizontal run keeps the node count small. */
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

/* Centre of each resize handle, in widget coordinates. */
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

  /* Outline of the size the picture will become while a handle is dragged. */
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

/* One-device-pixel lines between image pixels, only when zoomed in enough
 * (4x and up) for them to help instead of clutter. Two repeating gradients
 * keep it to two render nodes however large the picture is. */
static void
draw_grid (EaselCanvas *self, GtkSnapshot *snapshot, const graphene_rect_t *bounds, double s)
{
  if (!self->show_grid || self->zoom < 400)
    return;

  float step = (float) s;
  float line = 1.0f / (float) gtk_widget_get_scale_factor (GTK_WIDGET (self));
  float cut = line / step;
  GdkRGBA ink = { 0.45f, 0.45f, 0.5f, 0.55f }, none = { 0.45f, 0.45f, 0.5f, 0 };
  GskColorStop stops[4] = { { 0, ink }, { cut, ink }, { cut, none }, { 1, none } };

  graphene_point_t o = GRAPHENE_POINT_INIT (0, 0);
  graphene_point_t right = GRAPHENE_POINT_INIT (step, 0), down = GRAPHENE_POINT_INIT (0, step);

  gtk_snapshot_append_repeating_linear_gradient (snapshot, bounds, &o, &right, stops, 4);
  gtk_snapshot_append_repeating_linear_gradient (snapshot, bounds, &o, &down, stops, 4);
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
  /* Transparent pixels show a checkerboard behind them. */
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

  /* A hairline edge plus a soft shadow, hugging the picture itself (the
   * widget is a little larger to leave room for the resize handles). */
  {
    GskRoundedRect outline;
    const GdkRGBA edge = { 0, 0, 0, 0.30f };
    const GdkRGBA soft = { 0, 0, 0, 0.18f };

    gsk_rounded_rect_init_from_rect (&outline, &bounds, 0);
    gtk_snapshot_append_outset_shadow (snapshot, &outline, &soft, 0, 1, 0, 6);
    gtk_snapshot_append_outset_shadow (snapshot, &outline, &edge, 0, 0, 0.5f, 0);
  }

  gtk_snapshot_append_scaled_texture (snapshot, self->texture,
                                      self->zoom < 100 ? GSK_SCALING_FILTER_TRILINEAR
                                                       : GSK_SCALING_FILTER_NEAREST, &bounds);

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
  /* Picking outside the image yields white, as classic Paint does. */
  if (x < 0 || y < 0 || x >= self->doc->width || y >= self->doc->height)
    return EASEL_RGB (255, 255, 255);
  uint32_t c = easel_document_get_pixel (self->doc, x, y);
  if ((c >> 24) != 0xFF) /* see-through pixel: pick what is seen over white */
    {
      uint32_t miss = 255 - (c >> 24);
      return EASEL_RGB (MIN (((c >> 16) & 0xFF) + miss, 255u), MIN (((c >> 8) & 0xFF) + miss, 255u),
                        MIN ((c & 0xFF) + miss, 255u));
    }
  return c;
}

/* Records the finished edit (whose "before" picture is `base`) for undo. */
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

/* Pointer input is handled by canvas_press / canvas_drag / canvas_release,
 * which take plain values (button, position, modifiers) rather than a GTK
 * gesture, so the same code can be driven by tests. The on_drag_* functions
 * below only translate gesture signals into these calls. */
void
canvas_press (EaselCanvas *self, guint button, double x, double y, gboolean ctrl)
{
  if (button == GDK_BUTTON_PRIMARY)
    {
      int handle = handle_at (self, x, y);
      if (handle != 0)
        {
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
          return;
        }
    }

  /* Right-click on a selection or on the text box opens its menu (on release). */
  if (button == GDK_BUTTON_SECONDARY)
    {
      int ix, iy, kind = -1;

      to_image (self, x, y, &ix, &iy);
      if ((easel_tool_info (self->tool)->flags & EASEL_TOOL_SELECTION) && canvas_sel_contains (self, ix, iy))
        kind = 0;
      else if (self->tool == EASEL_TOOL_TEXT && self->text_active
               && canvas_text_hit (self, x, y) != TEXT_HIT_NONE)
        kind = 1;
      if (kind >= 0 && self->ctx_model[kind] != NULL)
        {
          self->ctx_pending = kind;
          self->ctx_x = x;
          self->ctx_y = y;
          return;
        }
    }

  /* Left button uses the foreground colour, right the background. */
  self->button = button;
  self->stroke_color = button == GDK_BUTTON_PRIMARY ? self->foreground : self->background;
  self->start_x = x;
  self->start_y = y;
  to_image (self, x, y, &self->last_x, &self->last_y);
  self->start_ix = self->last_x;
  self->start_iy = self->last_y;

  switch (self->tool)
    {
    case EASEL_TOOL_PAN:
      {
        GtkWidget *sw = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_SCROLLED_WINDOW);
        graphene_point_t in = GRAPHENE_POINT_INIT ((float) x, (float) y), out;

        if (sw != NULL && gtk_widget_compute_point (GTK_WIDGET (self), sw, &in, &out))
          {
            self->panning = TRUE;
            self->drawing = TRUE;
            self->pan_x0 = out.x;
            self->pan_y0 = out.y;
            self->pan_h0 = gtk_adjustment_get_value (gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (sw)));
            self->pan_v0 = gtk_adjustment_get_value (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (sw)));
            gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "grabbing");
          }
      }
      break;

    case EASEL_TOOL_TEXT:
      gtk_widget_grab_focus (GTK_WIDGET (self));
      self->drawing = TRUE;
      if (canvas_text_hit (self, x, y) >= 1 && canvas_text_hit (self, x, y) <= TEXT_HIT_BORDER)
        {
          int hit = canvas_text_hit (self, x, y);

          self->text_orig = self->text_rect;
          if (hit == TEXT_HIT_BORDER)
            {
              self->text_drag = TEXT_DRAG_MOVE;
              self->text_grab_x = self->last_x - self->text_rect.x;
              self->text_grab_y = self->last_y - self->text_rect.y;
            }
          else
            {
              self->text_drag = TEXT_DRAG_RESIZE;
              self->text_handle = hit;
            }
        }
      else if (canvas_text_hit (self, x, y) == TEXT_HIT_INSIDE)
        {
          /* Click inside the box places the caret; dragging selects. */
          canvas_text_move_cursor (self, canvas_text_index_at (self, self->last_x, self->last_y), FALSE);
          self->text_drag = TEXT_DRAG_SELECT;
        }
      else
        canvas_text_begin_drag (self, self->last_x, self->last_y);
      break;

    case EASEL_TOOL_SELECT:
    case EASEL_TOOL_FREE_FORM_SELECT:
      self->drawing = TRUE;
      if ((self->sel_stretch = canvas_sel_handle_at (self, x, y)) != 0)
        {
          canvas_sel_lift (self, FALSE);
          if (self->sel_clip == NULL)
            self->sel_stretch = 0;
          else
            {
              g_clear_pointer (&self->sel_src, easel_clip_free);
              self->sel_src = easel_clip_scaled (self->sel_clip, self->sel_clip->width, self->sel_clip->height);
              self->sel_src_rect = self->sel_rect;
              if (self->sel_src == NULL)
                self->sel_stretch = 0;
            }
        }
      else if (canvas_sel_contains (self, self->last_x, self->last_y))
        {
          canvas_sel_lift (self, ctrl);
          self->sel_moving = TRUE;
          self->sel_grab_x = self->last_x - self->sel_rect.x;
          self->sel_grab_y = self->last_y - self->sel_rect.y;
        }
      else
        {
          canvas_sel_commit (self);
          self->sel_creating = TRUE;
          self->sel_rect = canvas_sel_rect_between (self, self->last_x, self->last_y, self->last_x, self->last_y);
          if (self->tool == EASEL_TOOL_FREE_FORM_SELECT)
            {
              EaselPoint p = { self->sel_rect.x, self->sel_rect.y };
              self->sel_path = g_array_new (FALSE, FALSE, sizeof (EaselPoint));
              g_array_append_val (self->sel_path, p);
              self->sel_path_x = self->sel_path_y = 0;
            }
        }
      gtk_widget_queue_draw (GTK_WIDGET (self));
      break;

    case EASEL_TOOL_MAGNIFIER:
      easel_canvas_set_zoom (self, self->zoom == 100 ? self->zoom_level * 100 : 100,
                             self->last_x, self->last_y);
      break;

    case EASEL_TOOL_AIRBRUSH:
      self->drawing = TRUE;
      canvas_begin_edit (self);
      canvas_spray_tick (self);
      g_clear_handle_id (&self->spray_source, g_source_remove);
      self->spray_source = g_timeout_add (EASEL_SPRAY_INTERVAL_MS, canvas_spray_tick, self);
      break;

    case EASEL_TOOL_POLYGON:
      {
        gint64 now = g_get_monotonic_time ();

        gtk_widget_grab_focus (GTK_WIDGET (self));
        if (!self->seq_active)
          {
            canvas_snapshot_drop (self, &self->backup);
            self->backup = canvas_snapshot_take (self);
            self->shape_prev = (EaselRect) { 0, 0, 0, 0 };
            self->seq_active = TRUE;
            self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
            self->seq[0] = self->seq[1] = (EaselPoint) { self->last_x, self->last_y };
            self->seq_n = 2;
          }
        else
          {
            EaselPoint first = self->seq[0], tail = self->seq[self->seq_n - 1];
            gboolean dbl = now - self->last_press_us < 400000
                           && self->last_x == tail.x && self->last_y == tail.y;
            gboolean near_start = self->seq_n >= 3 && abs (self->last_x - first.x) <= 3
                                  && abs (self->last_y - first.y) <= 3;

            if (dbl || near_start)
              {
                canvas_commit_seq (self, TRUE); /* closes and fills */
                break;
              }
            if (self->seq_n < MAX_SEQ)
              self->seq[self->seq_n++] = (EaselPoint) { self->last_x, self->last_y };
          }
        self->last_press_us = now;
        self->drawing = TRUE;
        canvas_seq_render (self, FALSE, FALSE, 0, 0);
        break;
      }

    case EASEL_TOOL_CURVE:
      {
        EaselPoint here = { self->last_x, self->last_y };

        gtk_widget_grab_focus (GTK_WIDGET (self));
        if (!self->seq_active)
          {
            canvas_snapshot_drop (self, &self->backup);
            self->backup = canvas_snapshot_take (self);
            self->shape_prev = (EaselRect) { 0, 0, 0, 0 };
            self->seq_active = TRUE;
            self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
            self->seq_stage = 0;
            self->seq[0] = self->seq[1] = here;
          }
        else if (self->seq_stage == 1)
          self->seq[2] = self->seq[3] = here;
        else
          self->seq[3] = here;
        self->seq_moved = FALSE;
        self->drawing = TRUE;
        canvas_seq_render (self, FALSE, FALSE, 0, 0);
        break;
      }

    case EASEL_TOOL_FILL:
      canvas_begin_edit (self);
      canvas_mark_dirty (self, easel_raster_flood_fill (self->doc, self->last_x, self->last_y,
                                                 self->stroke_color));
      canvas_finish_edit (self);
      break;

    case EASEL_TOOL_PICK_COLOR:
      self->picked = sample (self, self->last_x, self->last_y);
      self->drawing = TRUE; /* resolved on release */
      break;

    case EASEL_TOOL_GRADIENT:
      gtk_widget_grab_focus (GTK_WIDGET (self));
      if (self->grad_pending)
        {
          double s = canvas_logical_per_pixel (self);
          int hit = 0;

          if (fabs (x - (self->grad_x0 + 0.5) * s) <= 7 && fabs (y - (self->grad_y0 + 0.5) * s) <= 7)
            hit = 1;
          else if (fabs (x - (self->grad_x1 + 0.5) * s) <= 7 && fabs (y - (self->grad_y1 + 0.5) * s) <= 7)
            hit = 2;
          if (hit != 0)
            {
              self->grad_drag = hit;
              self->drawing = TRUE;
              break;
            }
          canvas_grad_commit (self); /* a press elsewhere accepts it and starts a new one */
        }
      /* Fills the selection if there is one, otherwise the whole picture. */
      if (self->sel_active && self->sel_clip != NULL)
        canvas_sel_commit (self);
      self->grad_region = self->sel_active ? self->sel_rect
                                           : (EaselRect) { 0, 0, self->doc->width, self->doc->height };
      self->grad_masked = self->sel_active && self->sel_mask != NULL;
      /* fall through */
    case EASEL_TOOL_LINE:
    case EASEL_TOOL_RECTANGLE:
    case EASEL_TOOL_ELLIPSE:
    case EASEL_TOOL_ROUNDED_RECTANGLE:
      self->drawing = TRUE;
      self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
      canvas_snapshot_drop (self, &self->backup);
      self->backup = canvas_snapshot_take (self);
      self->shape_prev = (EaselRect) { 0, 0, 0, 0 };
      break;

    case EASEL_TOOL_PENCIL:
    case EASEL_TOOL_BRUSH:
    case EASEL_TOOL_ERASER:
      self->drawing = TRUE;
      canvas_begin_edit (self);
      canvas_mark_dirty (self, canvas_apply_stroke (self, self->last_x, self->last_y,
                                      self->last_x, self->last_y));
      break;

    default:
      break;
    }
}

void
canvas_drag (EaselCanvas *self, double offset_x, double offset_y, gboolean shift)
{
  int x, y;

  if (self->resize_handle != 0)
    {
      double s = canvas_logical_per_pixel (self);
      int nx = (int) lround ((self->start_x + offset_x) / s);
      int ny = (int) lround ((self->start_y + offset_y) / s);

      /* The handle sits just past the edge, so measure from the start point. */
      nx = self->doc->width + (nx - (int) lround (self->start_x / s));
      ny = self->doc->height + (ny - (int) lround (self->start_y / s));
      if (self->resize_handle != 2)
        self->resize_w = CLAMP (nx, 1, EASEL_MAX_DIMENSION);
      if (self->resize_handle != 1)
        self->resize_h = CLAMP (ny, 1, EASEL_MAX_DIMENSION);
      g_signal_emit (self, canvas_signals[SIGNAL_RESIZE_PREVIEW], 0, self->resize_w, self->resize_h);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

  if (!self->drawing)
    return;

  if (self->panning)
    {
      /* Measured against the scroller, which does not move while the picture does. */
      GtkWidget *sw = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_SCROLLED_WINDOW);
      graphene_point_t in = GRAPHENE_POINT_INIT ((float) (self->start_x + offset_x), (float) (self->start_y + offset_y)), out;

      if (sw != NULL && gtk_widget_compute_point (GTK_WIDGET (self), sw, &in, &out))
        {
          gtk_adjustment_set_value (gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (sw)),
                                    self->pan_h0 - (out.x - self->pan_x0));
          gtk_adjustment_set_value (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (sw)),
                                    self->pan_v0 - (out.y - self->pan_y0));
        }
      return;
    }

  to_image (self, self->start_x + offset_x, self->start_y + offset_y, &x, &y);
  if (self->grad_drag != 0)
    {
      if (self->grad_drag == 1)
        {
          self->grad_x0 = x;
          self->grad_y0 = y;
        }
      else
        {
          self->grad_x1 = x;
          self->grad_y1 = y;
        }
      canvas_grad_render (self);
      return;
    }
  if (x == self->last_x && y == self->last_y)
    return;

  if (self->tool == EASEL_TOOL_PICK_COLOR)
    self->picked = sample (self, x, y);
  else if ((easel_tool_info (self->tool)->flags & EASEL_TOOL_SHAPE))
    canvas_update_shape (self, x, y, shift);
  else if (self->tool == EASEL_TOOL_POLYGON)
    {
      self->seq[self->seq_n - 1] = (EaselPoint) { x, y };
      canvas_seq_render (self, FALSE, FALSE, 0, 0);
    }
  else if (self->tool == EASEL_TOOL_CURVE)
    {
      EaselPoint here = { x, y };

      if (self->seq_stage == 0)
        self->seq[1] = here;
      else if (self->seq_stage == 1)
        self->seq[2] = self->seq[3] = here;
      else
        self->seq[3] = here;
      self->seq_moved = TRUE;
      canvas_seq_render (self, FALSE, FALSE, 0, 0);
    }
  else if (self->tool == EASEL_TOOL_TEXT)
    {
      if (self->text_drag == TEXT_DRAG_MOVE)
        {
          self->text_rect.x = x - self->text_grab_x;
          self->text_rect.y = y - self->text_grab_y;
          gtk_widget_queue_draw (GTK_WIDGET (self));
        }
      else if (self->text_drag == TEXT_DRAG_RESIZE)
        canvas_text_resize_to (self, x, y);
      else if (self->text_drag == TEXT_DRAG_SELECT)
        canvas_text_move_cursor (self, canvas_text_index_at (self, x, y), TRUE);
      else if (self->text_creating)
        canvas_text_drag_to (self, self->start_ix, self->start_iy, x, y);
    }
  else if ((easel_tool_info (self->tool)->flags & EASEL_TOOL_SELECTION))
    {
      if (self->sel_stretch != 0)
        canvas_sel_stretch_to (self, x, y);
      else if (self->sel_moving)
        {
          self->sel_rect.x = x - self->sel_grab_x;
          self->sel_rect.y = y - self->sel_grab_y;
        }
      else if (self->sel_creating && self->sel_path != NULL)
        {
          EaselPoint p = { CLAMP (x, 0, self->doc->width - 1), CLAMP (y, 0, self->doc->height - 1) };
          EaselPoint last = g_array_index (self->sel_path, EaselPoint, self->sel_path->len - 1);

          if (p.x != last.x || p.y != last.y)
            g_array_append_val (self->sel_path, p);
          int x0 = p.x, y0 = p.y, x1 = p.x, y1 = p.y;
          for (guint i = 0; i < self->sel_path->len; i++)
            {
              EaselPoint q = g_array_index (self->sel_path, EaselPoint, i);
              x0 = MIN (x0, q.x); y0 = MIN (y0, q.y); x1 = MAX (x1, q.x); y1 = MAX (y1, q.y);
            }
          self->sel_rect = (EaselRect) { x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
        }
      else if (self->sel_creating)
        self->sel_rect = canvas_sel_rect_between (self, self->start_ix, self->start_iy, x, y);
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
  else if (self->tool == EASEL_TOOL_AIRBRUSH)
    ; /* the timer sprays at last_x/last_y */
  else
    /* Join consecutive samples with a line so fast strokes have no gaps. */
    canvas_mark_dirty (self, canvas_apply_stroke (self, self->last_x, self->last_y, x, y));

  self->last_x = x;
  self->last_y = y;
}

void
canvas_release (EaselCanvas *self, double offset_x, double offset_y, gboolean shift)
{
  if (self->ctx_pending >= 0)
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
      return;
    }

  if (self->resize_handle != 0)
    {
      self->resize_handle = 0;
      self->handle_hover = -1; /* next motion re-picks the cursor */
      gtk_widget_set_cursor (GTK_WIDGET (self), easel_cursor_for_tool (self->tool));
      g_signal_emit (self, canvas_signals[SIGNAL_RESIZE_PREVIEW], 0, -1, -1);
      if (self->resize_w != self->doc->width || self->resize_h != self->doc->height)
        easel_canvas_resize (self, self->resize_w, self->resize_h);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

  if (!self->drawing)
    return;
  self->drawing = FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self)); /* hover preview comes back */

  if (self->panning)
    {
      self->panning = FALSE;
      gtk_widget_set_cursor (GTK_WIDGET (self), easel_cursor_for_tool (self->tool));
      return;
    }

  if (self->tool == EASEL_TOOL_TEXT)
    {
      if (self->text_creating)
        canvas_text_end_drag (self);
      self->text_drag = TEXT_DRAG_NONE;
      return;
    }

  if ((easel_tool_info (self->tool)->flags & EASEL_TOOL_SELECTION))
    {
      if (self->sel_creating && self->sel_path != NULL)
        {
          self->sel_creating = FALSE;
          if (self->sel_path->len >= 3)
            {
              self->sel_mask = easel_raster_polygon_mask ((EaselPoint *) (void *) self->sel_path->data,
                                                          (int) self->sel_path->len, self->sel_rect);
              self->sel_path_x = self->sel_rect.x;
              self->sel_path_y = self->sel_rect.y;
              self->sel_active = self->sel_mask != NULL;
            }
          if (!self->sel_active)
            {
              g_clear_pointer (&self->sel_path, g_array_unref);
              gtk_widget_queue_draw (GTK_WIDGET (self));
            }
          canvas_sel_notify (self);
        }
      else if (self->sel_creating)
        {
          int x, y;
          to_image (self, self->start_x + offset_x, self->start_y + offset_y, &x, &y);
          self->sel_creating = FALSE;
          /* A click without dragging just deselects. */
          self->sel_active = !(x == self->start_ix && y == self->start_iy);
          if (!self->sel_active)
            gtk_widget_queue_draw (GTK_WIDGET (self));
          canvas_sel_notify (self);
        }
      self->sel_moving = FALSE;
      self->sel_stretch = 0;
      g_clear_pointer (&self->sel_src, easel_clip_free);
      return;
    }

  if (self->tool == EASEL_TOOL_AIRBRUSH)
    {
      g_clear_handle_id (&self->spray_source, g_source_remove);
      canvas_finish_edit (self);
      return;
    }

  if (self->tool == EASEL_TOOL_POLYGON)
    {
      int x, y;
      to_image (self, self->start_x + offset_x, self->start_y + offset_y, &x, &y);
      if (self->seq_active)
        {
          self->seq[self->seq_n - 1] = (EaselPoint) { x, y };
          if (self->seq_n == 2 && x == self->seq[0].x && y == self->seq[0].y)
            self->seq_n = 1; /* first press without a drag: the next click sets the first edge's end */
          canvas_seq_render (self, FALSE, FALSE, 0, 0);
        }
      return;
    }

  if (self->tool == EASEL_TOOL_CURVE)
    {
      if (self->seq_stage == 0)
        {
          if (self->seq[1].x == self->seq[0].x && self->seq[1].y == self->seq[0].y)
            canvas_cancel_seq (self);
          else
            self->seq_stage = 1;
        }
      else if (self->seq_stage == 1)
        {
          if (self->seq_moved)
            self->seq_stage = 2;
          else
            {
              self->seq[2] = self->seq[0];
              self->seq[3] = self->seq[1];
              canvas_commit_seq (self, FALSE);
            }
        }
      else
        {
          if (!self->seq_moved)
            self->seq[3] = self->seq[2];
          canvas_commit_seq (self, FALSE);
        }
      return;
    }

  if ((easel_tool_info (self->tool)->flags & EASEL_TOOL_SHAPE))
    {
      int x, y;
      to_image (self, self->start_x + offset_x, self->start_y + offset_y, &x, &y);
      /* A plain click (no movement) draws nothing. */
      if (self->grad_drag != 0)
        {
          self->grad_drag = 0; /* still adjustable */
          return;
        }
      if (x == self->start_ix && y == self->start_iy)
        canvas_end_shape_drag (self, FALSE);
      else if (self->tool == EASEL_TOOL_GRADIENT)
        {
          canvas_update_shape (self, x, y, shift);
          self->grad_pending = TRUE; /* kept until accepted: Enter, another press, a tool change */
        }
      else
        {
          canvas_update_shape (self, x, y, shift);
          canvas_end_shape_drag (self, TRUE);
        }
      return;
    }

  if (self->tool == EASEL_TOOL_PICK_COLOR)
    {
      int x, y;
      to_image (self, self->start_x + offset_x, self->start_y + offset_y, &x, &y);
      self->picked = sample (self, x, y);
      g_signal_emit (self, canvas_signals[SIGNAL_COLOR_PICKED], 0,
                     (guint) self->picked, self->button == GDK_BUTTON_PRIMARY);
    }

  canvas_finish_edit (self); /* pencil, brush, eraser */
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
  /* Over a selection the pointer offers to move it. */
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

  /* While a handle is being dragged the pointer keeps its resize cursor. */
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
  for (int i = 0; i < 2; i++)
    {
      g_clear_pointer (&self->ctx_popover[i], gtk_widget_unparent);
      g_clear_object (&self->ctx_model[i]);
    }
  g_clear_object (&self->texture);
  g_clear_pointer (&self->dirty, cairo_region_destroy);
  canvas_snapshot_drop (self, &self->backup);
  g_clear_handle_id (&self->spray_source, g_source_remove);
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

/* Menus for right-clicking a selection and the text box; the canvas shows
 * them, the window supplies the actions. */
void
easel_canvas_set_context_menus (EaselCanvas *self, GMenuModel *selection, GMenuModel *text)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  GMenuModel *models[2] = { selection, text };

  for (int i = 0; i < 2; i++)
    {
      g_clear_pointer (&self->ctx_popover[i], gtk_widget_unparent);
      g_set_object (&self->ctx_model[i], models[i]);
    }
}

/* One notch in (+1) or out (-1); (ix, iy) is the picture pixel to keep in
 * view, or negative for the middle of the window. */
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
  self->history = easel_history_new (EASEL_UNDO_LIMIT_BYTES);
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

/* Sets the magnification and keeps image point (ix, iy) in the middle of the
 * view; ix < 0 keeps whatever is in the middle now. */
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
    canvas_sel_refresh_texture (self); /* checkerboard cell size follows the zoom */
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
  if (self->seq_active) /* Undo during a polygon or curve abandons it */
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

/* Fills the whole image (Image > Clear Image) as one undoable step. */
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

/* Replaces the picture (File > Open, New). Takes ownership of `doc`; undo
 * history and any tool in progress are discarded. */
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

/* Changes the picture size (crop or extend with the background colour) as one
 * undoable step. */
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
easel_canvas_set_show_grid (EaselCanvas *self, gboolean show)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->show_grid = show;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
