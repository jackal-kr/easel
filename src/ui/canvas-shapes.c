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

/* Starts a polygon or curve; allocates the point array the first time. */
void
canvas_seq_begin (EaselCanvas *self)
{
  if (self->seq == NULL)
    self->seq = g_new0 (EaselPoint, MAX_SEQ);
  self->seq_active = TRUE;
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
  /* Starts in the chosen colour and fades to transparent, so a second
   * gradient lays over the first instead of replacing it. */
  uint32_t c0 = primary ? self->foreground : self->background;
  uint32_t c1 = 0;

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

/* ---- Mouse input for the shape tools ----------------------------------- */

/* Line, Rectangle, Ellipse, Rounded Rectangle and Gradient: the picture is
 * saved on press, the shape is redrawn over it on every drag, and the release
 * keeps it. */
static void
shape_press (EaselCanvas *self, double x, double y, gboolean ctrl)
{
  self->drawing = TRUE;
  self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
  canvas_snapshot_drop (self, &self->backup);
  self->backup = canvas_snapshot_take (self);
  self->shape_prev = (EaselRect) { 0, 0, 0, 0 };
}

static void
shape_drag (EaselCanvas *self, int x, int y, gboolean shift)
{
  canvas_update_shape (self, x, y, shift);
}

static void
shape_release (EaselCanvas *self, int x, int y, gboolean shift)
{
  /* A click with no movement doesn't make a gradient. */
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
      self->grad_pending = TRUE; /* stays editable until Enter, another click elsewhere, or a tool change */
    }
  else
    {
      canvas_update_shape (self, x, y, shift);
      canvas_end_shape_drag (self, TRUE);
    }
}

/* The gradient stays adjustable after it is drawn: a press on one of its two
 * end points drags that point, a press anywhere else accepts it and starts
 * a new one. */
static void
gradient_press (EaselCanvas *self, double x, double y, gboolean ctrl)
{
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
          return;
        }
      canvas_grad_commit (self); /* a press elsewhere accepts it and starts a new one */
    }
  /* Fills the selection if there is one, otherwise the whole picture. */
  if (self->sel_active && self->sel_clip != NULL)
    canvas_sel_commit (self);
  self->grad_region = self->sel_active ? self->sel_rect
                                       : (EaselRect) { 0, 0, self->doc->width, self->doc->height };
  self->grad_masked = self->sel_active && self->sel_mask != NULL;
  shape_press (self, x, y, ctrl);
}

/* Dragging one of the gradient's end points. */
void
canvas_grad_drag_to (EaselCanvas *self, int x, int y)
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
}

/* Polygon: every click adds a corner. Double-click, or a click near the first
 * corner, closes and fills it. */
static void
polygon_press (EaselCanvas *self, double x, double y, gboolean ctrl)
{
  gint64 now = g_get_monotonic_time ();

  gtk_widget_grab_focus (GTK_WIDGET (self));
  if (!self->seq_active)
    {
      canvas_snapshot_drop (self, &self->backup);
      self->backup = canvas_snapshot_take (self);
      self->shape_prev = (EaselRect) { 0, 0, 0, 0 };
      canvas_seq_begin (self);
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
          return;
        }
      if (self->seq_n < MAX_SEQ)
        self->seq[self->seq_n++] = (EaselPoint) { self->last_x, self->last_y };
    }
  self->last_press_us = now;
  self->drawing = TRUE;
  canvas_seq_render (self, FALSE, FALSE, 0, 0);
}

static void
polygon_drag (EaselCanvas *self, int x, int y, gboolean shift)
{
  self->seq[self->seq_n - 1] = (EaselPoint) { x, y };
  canvas_seq_render (self, FALSE, FALSE, 0, 0);
}

static void
polygon_release (EaselCanvas *self, int x, int y, gboolean shift)
{
  if (!self->seq_active)
    return;
  self->seq[self->seq_n - 1] = (EaselPoint) { x, y };
  if (self->seq_n == 2 && x == self->seq[0].x && y == self->seq[0].y)
    self->seq_n = 1; /* no drag on the first click: the next click gives the end of the first edge */
  canvas_seq_render (self, FALSE, FALSE, 0, 0);
}

/* Curve: drag a line, then drag out one or two bends (stages 0, 1, 2). */
static void
curve_press (EaselCanvas *self, double x, double y, gboolean ctrl)
{
  EaselPoint here = { self->last_x, self->last_y };

  gtk_widget_grab_focus (GTK_WIDGET (self));
  if (!self->seq_active)
    {
      canvas_snapshot_drop (self, &self->backup);
      self->backup = canvas_snapshot_take (self);
      self->shape_prev = (EaselRect) { 0, 0, 0, 0 };
      canvas_seq_begin (self);
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
}

static void
curve_drag (EaselCanvas *self, int x, int y, gboolean shift)
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

static void
curve_release (EaselCanvas *self, int x, int y, gboolean shift)
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
}

const CanvasToolInput canvas_input_shape = { shape_press, shape_drag, shape_release };
const CanvasToolInput canvas_input_gradient = { gradient_press, shape_drag, shape_release };
const CanvasToolInput canvas_input_polygon = { polygon_press, polygon_drag, polygon_release };
const CanvasToolInput canvas_input_curve = { curve_press, curve_drag, curve_release };
