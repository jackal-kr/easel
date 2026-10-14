#include "ui/canvas-private.h"

/* Floating contents plus a dashed outline (white under black, so it shows on
 * any colour). */
void
canvas_draw_selection (EaselCanvas *self, GtkSnapshot *snapshot, double s)
{
  if (!self->sel_active && !self->sel_creating)
    return;

  EaselRect r = self->sel_rect;

  if (self->sel_texture != NULL)
    {
      graphene_rect_t at = GRAPHENE_RECT_INIT ((float) (r.x * s), (float) (r.y * s),
                                               (float) (r.width * s), (float) (r.height * s));
      gtk_snapshot_append_scaled_texture (snapshot, self->sel_texture, GSK_SCALING_FILTER_NEAREST, &at);
    }

  GskPathBuilder *builder = gsk_path_builder_new ();
  if (self->sel_path != NULL)
    {
      int ox = self->sel_creating ? 0 : r.x - self->sel_path_x;
      int oy = self->sel_creating ? 0 : r.y - self->sel_path_y;

      for (guint i = 0; i < self->sel_path->len; i++)
        {
          EaselPoint q = g_array_index (self->sel_path, EaselPoint, i);
          float qx = (float) ((q.x + ox + 0.5) * s), qy = (float) ((q.y + oy + 0.5) * s);
          if (i == 0)
            gsk_path_builder_move_to (builder, qx, qy);
          else
            gsk_path_builder_line_to (builder, qx, qy);
        }
      if (!self->sel_creating)
        gsk_path_builder_close (builder);
    }
  else
    {
      graphene_rect_t box = GRAPHENE_RECT_INIT ((float) (r.x * s) + 0.5f, (float) (r.y * s) + 0.5f,
                                                (float) (r.width * s) - 1, (float) (r.height * s) - 1);
      gsk_path_builder_add_rect (builder, &box);
    }
  GskPath *path = gsk_path_builder_free_to_path (builder);

  const GdkRGBA white = { 1, 1, 1, 1 }, black = { 0, 0, 0, 1 };
  GskStroke *solid = gsk_stroke_new (1.0f);
  GskStroke *dashed = gsk_stroke_new (1.0f);
  const float dashes[] = { 4, 4 };

  gsk_stroke_set_dash (dashed, dashes, 2);
  gtk_snapshot_append_stroke (snapshot, path, solid, &white);
  gtk_snapshot_append_stroke (snapshot, path, dashed, &black);
  gsk_stroke_free (solid);
  gsk_stroke_free (dashed);
  gsk_path_unref (path);

  if (self->sel_active && !self->sel_creating)
    for (int h = 1; h <= 8; h++)
      {
        double cx, cy;
        canvas_sel_handle_pos (&r, h, s, &cx, &cy);
        graphene_rect_t halo = GRAPHENE_RECT_INIT ((float) cx - 3.5f, (float) cy - 3.5f, 7, 7);
        graphene_rect_t dot = GRAPHENE_RECT_INIT ((float) cx - 2.5f, (float) cy - 2.5f, 5, 5);
        gtk_snapshot_append_color (snapshot, &white, &halo);
        gtk_snapshot_append_color (snapshot, &black, &dot);
      }
}

/* ---- Selection ------------------------------------------------------- */

GdkTexture *
canvas_texture_from_clip (const EaselClip *clip, gboolean opaque, uint32_t key, int checker)
{
  gsize n = (gsize) clip->width * clip->height;
  g_autofree uint32_t *argb = g_new (uint32_t, n);

  for (gsize i = 0; i < n; i++)
    {
      gboolean outside = clip->mask != NULL && !clip->mask[i];
      gboolean see_through = !opaque && clip->pixels[i] == key;

      if (outside)
        argb[i] = 0;
      else if (see_through)
        {
          /* Show transparency as a faint checkerboard, as image editors do. */
          int x = (int) (i % (gsize) clip->width), y = (int) (i / (gsize) clip->width);
          argb[i] = checker > 0 && ((x / checker + y / checker) & 1) ? 0x66464646u : 0;
        }
      else
        argb[i] = clip->pixels[i];
    }

  g_autoptr (GBytes) bytes = g_bytes_new (argb, n * 4);
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  GdkMemoryFormat format = GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
#else
  GdkMemoryFormat format = GDK_MEMORY_A8R8G8B8_PREMULTIPLIED;
#endif
  return gdk_memory_texture_new (clip->width, clip->height, format, bytes, (gsize) clip->width * 4);
}

void
canvas_sel_refresh_texture (EaselCanvas *self)
{
  g_clear_object (&self->sel_texture);
  if (self->sel_clip != NULL)
    self->sel_texture = canvas_texture_from_clip (self->sel_clip, self->sel_opaque, self->background,
                                           MAX (1, 800 / self->zoom));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
canvas_sel_notify (EaselCanvas *self)
{
  g_signal_emit (self, canvas_signals[SIGNAL_SELECTION_CHANGED], 0);
}

/* What cutting or lifting leaves behind: the background colour in Opaque
 * mode, true transparency (shown as a checkerboard) in Transparent mode. */
static uint32_t
hole_color (EaselCanvas *self)
{
  return self->sel_opaque ? self->background : 0;
}

/* First move of a selection: take its pixels off the picture. With `copy`
 * the original stays (Ctrl-drag stamps a copy). */
void
canvas_sel_lift (EaselCanvas *self, gboolean copy)
{
  if (self->sel_clip != NULL || !self->sel_active)
    return;

  self->sel_clip = easel_clip_copy_from (self->doc, self->sel_rect, self->sel_mask);
  if (self->sel_clip == NULL)
    return;
  canvas_snapshot_drop (self, &self->sel_base);
  self->sel_base = canvas_snapshot_take (self);
  self->sel_edit = (EaselRect) { 0, 0, 0, 0 };
  if (!copy)
    {
      self->sel_edit = easel_clip_erase (self->doc, self->sel_rect, self->sel_mask, hole_color (self));
      canvas_mark_dirty (self, self->sel_edit);
    }
  canvas_sel_refresh_texture (self);
}

void
canvas_sel_discard (EaselCanvas *self)
{
  g_clear_pointer (&self->sel_clip, easel_clip_free);
  canvas_snapshot_drop (self, &self->sel_base);
  g_clear_object (&self->sel_texture);
  g_clear_pointer (&self->sel_mask, g_free);
  g_clear_pointer (&self->sel_src, easel_clip_free);
  self->sel_stretch = 0;
  g_clear_pointer (&self->sel_path, g_array_unref);
  self->sel_active = self->sel_creating = self->sel_moving = FALSE;
  self->sel_edit = (EaselRect) { 0, 0, 0, 0 };
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* Puts a floating selection down (one undo step covering the lift and the
 * landing) and drops the selection. */
void
canvas_sel_commit (EaselCanvas *self)
{
  canvas_grad_commit (self);
  canvas_text_commit (self);
  if (!self->sel_active && !self->sel_creating)
    return;

  if (self->sel_clip != NULL)
    {
      EaselRect landed = easel_clip_blit (self->doc, self->sel_clip, self->sel_rect.x, self->sel_rect.y,
                                          !self->sel_opaque, self->background);
      self->edit_rect = easel_rect_union (self->sel_edit, landed);
      canvas_mark_dirty (self, landed);
      canvas_push_undo (self, self->sel_base);
    }
  canvas_sel_discard (self);
  canvas_sel_notify (self);
}

/* Drag corners to an image-clamped rectangle. */
EaselRect
canvas_sel_rect_between (EaselCanvas *self, int x0, int y0, int x1, int y1)
{
  int left = CLAMP (MIN (x0, x1), 0, self->doc->width - 1);
  int top = CLAMP (MIN (y0, y1), 0, self->doc->height - 1);
  int right = CLAMP (MAX (x0, x1), 0, self->doc->width - 1);
  int bottom = CLAMP (MAX (y0, y1), 0, self->doc->height - 1);

  return (EaselRect) { left, top, right - left + 1, bottom - top + 1 };
}

/* Stretch handles: 1 NW, 2 N, 3 NE, 4 W, 5 E, 6 SW, 7 S, 8 SE. */
void
canvas_sel_handle_pos (const EaselRect *r, int h, double s, double *wx, double *wy)
{
  int col = (h == 1 || h == 4 || h == 6) ? 0 : (h == 2 || h == 7) ? 1 : 2;
  int row = (h <= 3) ? 0 : (h <= 5) ? 1 : 2;

  *wx = (r->x + r->width * col / 2.0) * s;
  *wy = (r->y + r->height * row / 2.0) * s;
}

int
canvas_sel_handle_at (EaselCanvas *self, double wx, double wy)
{
  if (!self->sel_active || self->sel_creating)
    return 0;
  double s = canvas_logical_per_pixel (self);

  for (int h = 1; h <= 8; h++)
    {
      double cx, cy;
      canvas_sel_handle_pos (&self->sel_rect, h, s, &cx, &cy);
      if (fabs (wx - cx) <= 5 && fabs (wy - cy) <= 5)
        return h;
    }
  return 0;
}

void
canvas_sel_stretch_to (EaselCanvas *self, int x, int y)
{
  EaselRect o = self->sel_src_rect;
  int l = o.x, t = o.y, r = o.x + o.width, b = o.y + o.height, h = self->sel_stretch;

  if (h == 1 || h == 4 || h == 6) l = MIN (x, r - 1);
  if (h == 3 || h == 5 || h == 8) r = MAX (x + 1, l + 1);
  if (h <= 3) t = MIN (y, b - 1);
  if (h >= 6) b = MAX (y + 1, t + 1);
  if (r - l > 8192 || b - t > 8192)
    return;

  EaselClip *scaled = easel_clip_scaled (self->sel_src, r - l, b - t);
  if (scaled == NULL)
    return;
  easel_clip_free (self->sel_clip);
  self->sel_clip = scaled;
  self->sel_rect = (EaselRect) { l, t, r - l, b - t };
  g_clear_pointer (&self->sel_path, g_array_unref); /* outline is now the box */
  canvas_sel_refresh_texture (self);
}

gboolean
canvas_sel_contains (EaselCanvas *self, int x, int y)
{
  return self->sel_active && x >= self->sel_rect.x && y >= self->sel_rect.y
         && x < self->sel_rect.x + self->sel_rect.width && y < self->sel_rect.y + self->sel_rect.height;
}

/* ---- Selection API --------------------------------------------------- */

gboolean
easel_canvas_has_selection (EaselCanvas *self)
{
  g_return_val_if_fail (EASEL_IS_CANVAS (self), FALSE);
  int a, b;
  return self->sel_active || canvas_text_sel_range (self, &a, &b);
}

/* Puts any floating selection down; call before saving or other whole-picture work. */
void
easel_canvas_flush (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  canvas_text_commit (self);
  canvas_sel_commit (self);
  canvas_commit_seq (self, FALSE);
}

void
easel_canvas_set_eraser_opaque (EaselCanvas *self, gboolean opaque)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->eraser_opaque = opaque;
}

void
easel_canvas_set_opaque (EaselCanvas *self, gboolean opaque)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->sel_opaque = opaque;
  if (self->sel_clip != NULL)
    canvas_sel_refresh_texture (self);
}

void
easel_canvas_select_all (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  canvas_sel_commit (self);
  self->sel_rect = (EaselRect) { 0, 0, self->doc->width, self->doc->height };
  self->sel_active = TRUE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  canvas_sel_notify (self);
}

/* The selection's pixels, or NULL. */
static EaselClip *
selection_pixels (EaselCanvas *self)
{
  if (!self->sel_active)
    return NULL;
  if (self->sel_clip != NULL)
    {
      EaselClip *c = easel_clip_new (self->sel_clip->width, self->sel_clip->height);
      if (c != NULL)
        {
          memcpy (c->pixels, self->sel_clip->pixels, (gsize) c->width * c->height * 4);
          if (self->sel_clip->mask != NULL)
            c->mask = g_memdup2 (self->sel_clip->mask, (gsize) c->width * c->height);
        }
      return c;
    }
  return easel_clip_copy_from (self->doc, self->sel_rect, self->sel_mask);
}

EaselClip *
easel_canvas_get_selection (EaselCanvas *self)
{
  g_return_val_if_fail (EASEL_IS_CANVAS (self), NULL);
  return selection_pixels (self);
}

void
easel_canvas_copy (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  EaselClip *clip = selection_pixels (self);

  if (clip == NULL)
    return;
  g_autoptr (GdkTexture) texture = canvas_texture_from_clip (clip, TRUE, 0, 0);
  gdk_clipboard_set_texture (gtk_widget_get_clipboard (GTK_WIDGET (self)), texture);
  easel_clip_free (clip);
}

/* Delete / Cut: removes the selected pixels (background colour behind). */
void
easel_canvas_clear_selection (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (!self->sel_active)
    return;

  if (self->sel_clip != NULL)
    {
      /* Already lifted: the hole is there; just don't put the contents back. */
      g_clear_pointer (&self->sel_clip, easel_clip_free);
      g_clear_object (&self->sel_texture);
      self->edit_rect = self->sel_edit;
      canvas_push_undo (self, self->sel_base);
      canvas_sel_discard (self);
    }
  else
    {
      canvas_begin_edit (self);
      canvas_mark_dirty (self, easel_clip_erase (self->doc, self->sel_rect, self->sel_mask, hole_color (self)));
      canvas_finish_edit (self);
      canvas_sel_discard (self);
    }
  canvas_sel_notify (self);
}

void
easel_canvas_cut (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  easel_canvas_copy (self);
  easel_canvas_clear_selection (self);
}

static void
on_clipboard_texture (GObject *source, GAsyncResult *result, gpointer data)
{
  EaselCanvas *self = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GdkTexture) texture = gdk_clipboard_read_texture_finish (GDK_CLIPBOARD (source), result, &error);

  if (texture != NULL)
    {
      int w = gdk_texture_get_width (texture), h = gdk_texture_get_height (texture);
      EaselClip *clip = easel_clip_new (w, h);

      if (clip != NULL)
        {
          gdk_texture_download (texture, (guchar *) clip->pixels, (gsize) w * 4);
          /* Flatten onto white: premultiplied, so add the missing white. */
          for (gsize i = 0; i < (gsize) w * h; i++)
            {
              uint32_t c = clip->pixels[i];
              uint32_t miss = 255 - (c >> 24);
              uint32_t r = MIN (((c >> 16) & 0xFF) + miss, 255u);
              uint32_t g = MIN (((c >> 8) & 0xFF) + miss, 255u);
              uint32_t b = MIN ((c & 0xFF) + miss, 255u);
              clip->pixels[i] = EASEL_RGB (r, g, b);
            }
          easel_canvas_paste_clip (self, clip);
        }
    }
  g_object_unref (self);
}

void
easel_canvas_paste (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  gdk_clipboard_read_texture_async (gtk_widget_get_clipboard (GTK_WIDGET (self)), NULL,
                                    on_clipboard_texture, g_object_ref (self));
}

/* Floats `clip` (taking ownership) in the top-left of the visible area. The
 * picture grows if the clip does not fit. */
void
easel_canvas_paste_clip (EaselCanvas *self, EaselClip *clip)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  g_return_if_fail (clip != NULL);

  canvas_sel_commit (self);
  canvas_commit_seq (self, FALSE);
  if (clip->width > self->doc->width || clip->height > self->doc->height)
    easel_canvas_resize (self, MAX (clip->width, self->doc->width), MAX (clip->height, self->doc->height));

  int x = 0, y = 0;
  GtkWidget *sw = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_SCROLLED_WINDOW);
  if (sw != NULL)
    {
      double s = canvas_logical_per_pixel (self);
      x = (int) (gtk_adjustment_get_value (gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (sw))) / s);
      y = (int) (gtk_adjustment_get_value (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (sw))) / s);
    }

  canvas_snapshot_drop (self, &self->sel_base);
  self->sel_base = canvas_snapshot_take (self);
  self->sel_edit = (EaselRect) { 0, 0, 0, 0 };
  self->sel_clip = clip;
  self->sel_rect = (EaselRect) { x, y, clip->width, clip->height };
  self->sel_active = TRUE;
  canvas_sel_refresh_texture (self);
  canvas_sel_notify (self);
}
