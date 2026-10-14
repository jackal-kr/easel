#include "ui/canvas-private.h"

/* Pencil, brush or eraser along one segment of a stroke. When both ends are
 * the same point it's a single stamp, i.e. a click. */
EaselRect
canvas_apply_stroke (EaselCanvas *self, int x0, int y0, int x1, int y1)
{
  switch (self->tool)
    {
    case EASEL_TOOL_PENCIL:
    case EASEL_TOOL_BRUSH:
      {
        EaselBrush brush = {
          self->tool == EASEL_TOOL_BRUSH ? self->brush_shape : EASEL_BRUSH_CIRCLE,
          self->tool_size,
          self->stroke_color,
        };
        return easel_raster_stroke (self->doc, x0, y0, x1, y1, &brush);
      }
    case EASEL_TOOL_ERASER:
      {
        /* Left button wipes everything under it. Right button is the old
         * Paint "colour eraser": it only turns the foreground colour into the
         * background colour and leaves the rest alone. */
        EaselEraser eraser = {
          self->tool_size,
          self->eraser_opaque ? self->background : 0,
          self->button == GDK_BUTTON_SECONDARY,
          self->foreground,
        };
        return easel_raster_erase_stroke (self->doc, x0, y0, x1, y1, &eraser);
      }
    default:
      return (EaselRect) { 0, 0, 0, 0 };
    }
}

void
canvas_end_seq (EaselCanvas *self)
{
  self->seq_active = FALSE;
  self->seq_stage = 0;
  self->seq_n = 0;
  self->shape_prev = (EaselRect) { 0, 0, 0, 0 };
  canvas_snapshot_drop (self, &self->backup);
}

/* Polygon and curve are drawn over several clicks, so each time something
 * changes we restore the picture from `backup` and draw the whole shape again.
 * `rubber` adds the segment that follows the pointer. */
void
canvas_seq_render (EaselCanvas *self, gboolean closed, gboolean rubber, int rx, int ry)
{
  uint32_t outline = self->stroke_color;
  uint32_t fill = self->button == GDK_BUTTON_PRIMARY ? self->background : self->foreground;
  EaselRect now;

  canvas_restore_shape_area (self);
  if (self->tool == EASEL_TOOL_POLYGON)
    {
      EaselPoint pts[MAX_SEQ + 1];
      int n = self->seq_n;

      memcpy (pts, self->seq, (size_t) n * sizeof pts[0]);
      if (rubber && n < MAX_SEQ)
        pts[n++] = (EaselPoint) { rx, ry };
      now = closed && n >= 3
              ? easel_raster_polygon (self->doc, pts, n, self->line_width, self->fill_style, outline, fill)
              : easel_raster_polyline (self->doc, pts, n, self->line_width, outline);
    }
  else if (self->seq_stage == 0)
    now = easel_raster_line (self->doc, self->seq[0].x, self->seq[0].y, self->seq[1].x, self->seq[1].y,
                             self->line_width, outline);
  else
    now = easel_raster_curve (self->doc, self->seq[0], self->seq[2], self->seq[3], self->seq[1],
                              self->line_width, outline);

  canvas_mark_dirty (self, easel_rect_union (self->shape_prev, now));
  self->shape_prev = now;
}

void
canvas_cancel_seq (EaselCanvas *self)
{
  if (!self->seq_active)
    return;
  canvas_restore_shape_area (self);
  canvas_mark_dirty (self, self->shape_prev);
  canvas_end_seq (self);
  self->drawing = FALSE;
}

/* Finishes the polygon/curve: whatever is drawn now stays, minus the
 * segment that was following the pointer. `closed` joins last to first. */
void
canvas_commit_seq (EaselCanvas *self, gboolean closed)
{
  if (!self->seq_active)
    return;
  canvas_seq_render (self, closed, FALSE, 0, 0);
  canvas_push_undo (self, self->backup);
  canvas_end_seq (self);
  self->drawing = FALSE;
}

gboolean
canvas_spray_tick (gpointer data)
{
  EaselCanvas *self = data;
  int d = self->tool_size;

  canvas_mark_dirty (self, easel_raster_spray (self->doc, self->last_x, self->last_y, d,
                                        self->stroke_color, MAX (2, d * d / 8), &self->spray_rng));
  return G_SOURCE_CONTINUE;
}

/* Rubs out the preview of a shape by copying its area back from `backup`. */
void
canvas_restore_shape_area (EaselCanvas *self)
{
  EaselRect r = self->shape_prev;

  for (int y = r.y; y < r.y + r.height; y++)
    memcpy (&self->doc->pixels[(size_t) y * self->doc->width + r.x],
            &self->backup[(size_t) y * self->doc->width + r.x], (size_t) r.width * 4);
}

void
canvas_update_shape (EaselCanvas *self, int x, int y, gboolean constrain)
{
  int sx = self->start_ix, sy = self->start_iy;
  int dx = x - sx, dy = y - sy;
  int ax = abs (dx), ay = abs (dy);

  if (constrain)
    {
      if (self->tool == EASEL_TOOL_LINE || self->tool == EASEL_TOOL_GRADIENT)
        {
          if (ay * 2 < ax)
            dy = 0;
          else if (ax * 2 < ay)
            dx = 0;
          else
            {
              int m = MAX (ax, ay);
              dx = dx < 0 ? -m : m;
              dy = dy < 0 ? -m : m;
            }
        }
      else
        {
          int m = MAX (ax, ay);
          dx = dx < 0 ? -m : m;
          dy = dy < 0 ? -m : m;
        }
      x = sx + dx;
      y = sy + dy;
    }

  /* Left button: outline in the foreground colour, fill in the background
   * colour. Right button swaps them. */
  uint32_t outline = self->stroke_color;
  uint32_t fill = self->button == GDK_BUTTON_PRIMARY ? self->background : self->foreground;
  EaselRect now;

  canvas_restore_shape_area (self);
  switch (self->tool)
    {
    case EASEL_TOOL_LINE:
      now = easel_raster_line (self->doc, sx, sy, x, y, self->line_width, outline);
      break;
    case EASEL_TOOL_GRADIENT:
      self->grad_x0 = sx;
      self->grad_y0 = sy;
      self->grad_x1 = x;
      self->grad_y1 = y;
      self->grad_button = self->button;
      canvas_grad_render (self);
      now = self->shape_prev;
      break;
    case EASEL_TOOL_RECTANGLE:
      now = easel_raster_shape (self->doc, EASEL_SHAPE_RECT, sx, sy, x, y, self->line_width,
                                self->fill_style, outline, fill);
      break;
    case EASEL_TOOL_ROUNDED_RECTANGLE:
      now = easel_raster_shape (self->doc, EASEL_SHAPE_ROUNDED_RECT, sx, sy, x, y, self->line_width,
                                self->fill_style, outline, fill);
      break;
    default:
      now = easel_raster_shape (self->doc, EASEL_SHAPE_ELLIPSE, sx, sy, x, y, self->line_width,
                                self->fill_style, outline, fill);
      break;
    }

  canvas_mark_dirty (self, easel_rect_union (self->shape_prev, now));
  self->shape_prev = now;
}

/* (Re)draws the gradient between its two end points over the saved picture.
 * Called again every time an end point is dragged. */
void
canvas_grad_render (EaselCanvas *self)
{
  gboolean primary = self->grad_button == GDK_BUTTON_PRIMARY;
  uint32_t c0 = primary ? self->foreground : self->background;
  uint32_t c1 = primary ? self->background : self->foreground;

  canvas_restore_shape_area (self);
  EaselRect now = easel_raster_gradient (self->doc, self->grad_region, self->grad_masked ? self->sel_mask : NULL,
                                         self->grad_x0, self->grad_y0, self->grad_x1, self->grad_y1,
                                         (EaselGradientStyle) self->gradient_style, c0, c1);
  canvas_mark_dirty (self, easel_rect_union (self->shape_prev, now));
  self->shape_prev = now;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
canvas_end_shape_drag (EaselCanvas *self, gboolean keep)
{
  if (!keep)
    {
      canvas_restore_shape_area (self);
      canvas_mark_dirty (self, self->shape_prev);
      self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
    }
  else
    canvas_push_undo (self, self->backup);
  self->shape_prev = (EaselRect) { 0, 0, 0, 0 };
  canvas_snapshot_drop (self, &self->backup);
}

void
canvas_grad_commit (EaselCanvas *self)
{
  if (!self->grad_pending)
    return;
  self->grad_pending = FALSE;
  canvas_end_shape_drag (self, TRUE);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* The handles you can drag to adjust a gradient: a line with a dot at each end. */
void
canvas_draw_gradient_guide (EaselCanvas *self, GtkSnapshot *snapshot, double s)
{
  if (!self->grad_pending && !(self->drawing && self->tool == EASEL_TOOL_GRADIENT))
    return;

  float x0 = (float) ((self->grad_x0 + 0.5) * s), y0 = (float) ((self->grad_y0 + 0.5) * s);
  float x1 = (float) ((self->grad_x1 + 0.5) * s), y1 = (float) ((self->grad_y1 + 0.5) * s);
  const GdkRGBA white = { 1, 1, 1, 1 }, black = { 0, 0, 0, 1 };
  GskPathBuilder *builder = gsk_path_builder_new ();

  gsk_path_builder_move_to (builder, x0, y0);
  gsk_path_builder_line_to (builder, x1, y1);
  GskPath *path = gsk_path_builder_free_to_path (builder);
  GskStroke *wide = gsk_stroke_new (3.0f), *thin = gsk_stroke_new (1.0f);

  gtk_snapshot_append_stroke (snapshot, path, wide, &white);
  gtk_snapshot_append_stroke (snapshot, path, thin, &black);
  gsk_stroke_free (wide);
  gsk_stroke_free (thin);
  gsk_path_unref (path);

  for (int k = 0; k < 2; k++)
    {
      float cx = k == 0 ? x0 : x1, cy = k == 0 ? y0 : y1;
      graphene_rect_t halo = GRAPHENE_RECT_INIT (cx - 5, cy - 5, 10, 10);
      graphene_rect_t dot = GRAPHENE_RECT_INIT (cx - 3.5f, cy - 3.5f, 7, 7);

      gtk_snapshot_append_color (snapshot, &white, &halo);
      gtk_snapshot_append_color (snapshot, k == 0 ? &black : &white, &dot);
      if (k == 1)
        {
          graphene_rect_t core = GRAPHENE_RECT_INIT (cx - 2.5f, cy - 2.5f, 5, 5);
          gtk_snapshot_append_color (snapshot, &black, &core);
        }
    }
}

void
easel_canvas_set_gradient_style (EaselCanvas *self, int style)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->gradient_style = style;
}
