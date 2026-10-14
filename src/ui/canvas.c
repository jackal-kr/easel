#include "ui/canvas.h"

#include <math.h>
#include <string.h>

#include "core/history.h"
#include "ui/cursors.h"

#define MAX_SEQ 512 /* vertices of a polygon in progress */

struct _EaselCanvas
{
  GtkWidget parent_instance;

  EaselDocument  *doc;
  GdkTexture     *texture; /* last uploaded snapshot of doc */
  cairo_region_t *dirty;   /* image pixels changed since `texture` was built */

  int      zoom; /* integer magnification, 1 = actual size */
  uint32_t foreground;
  uint32_t background;

  EaselTool       tool;
  int             tool_size;
  EaselBrushShape brush_shape;
  int             line_width;
  EaselFillStyle  fill_style;

  /* Gesture in progress. */
  gboolean drawing;
  guint    button;       /* GDK_BUTTON_PRIMARY or _SECONDARY */
  uint32_t stroke_color; /* foreground for left button, background for right */
  uint32_t picked;       /* Pick Color: colour under the pointer */
  double   start_x, start_y; /* widget coordinates where the drag began */
  int      last_x, last_y;   /* last image pixel handled */
  int      start_ix, start_iy; /* image pixel where the drag began */
  uint32_t *backup;          /* pixels before a shape drag: lets the live preview be redrawn */
  EaselRect shape_prev;      /* area the previewed shape currently covers */

  /* Multi-step tools (polygon, curve): points collected so far. Polygon:
   * vertices. Curve: [0] start, [1] end, [2] and [3] control points. */
  gboolean   seq_active;
  int        seq_stage;     /* curve: 0 drawing the line, 1 awaiting bend, 2 awaiting 2nd bend */
  int        seq_n;
  EaselPoint seq[MAX_SEQ];
  gboolean   seq_moved;
  gint64     last_press_us;

  /* Undo */
  EaselHistory *history;
  EaselRect     edit_rect;  /* pixels touched by the edit in progress */
  uint32_t     *edit_base;  /* picture before a freehand edit (shape tools use `backup`) */

  /* Airbrush */
  guint    spray_source;
  uint32_t spray_rng;

  /* Magnifier */
  int    zoom_level;      /* zoom a click applies */
  gboolean center_pending;
  double center_x, center_y; /* image point to bring to the middle of the view */

  int hover_x, hover_y; /* pointer in image pixels, -1 when outside */
};

G_DEFINE_FINAL_TYPE (EaselCanvas, easel_canvas, GTK_TYPE_WIDGET)

enum { SIGNAL_POINTER_MOVED, SIGNAL_COLOR_PICKED, SIGNAL_HISTORY_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* One image pixel is one *device* pixel at zoom 1, even on HiDPI screens,
 * so the logical widget size is image size * zoom / scale factor. */
static double
logical_per_pixel (EaselCanvas *self)
{
  return (double) self->zoom / gtk_widget_get_scale_factor (GTK_WIDGET (self));
}

static void
to_image (EaselCanvas *self, double wx, double wy, int *ix, int *iy)
{
  double s = logical_per_pixel (self);
  *ix = (int) floor (wx / s);
  *iy = (int) floor (wy / s);
}

/* Records which image pixels changed so that only that region is re-uploaded
 * to the GPU (see build_texture). */
static void
mark_dirty (EaselCanvas *self, EaselRect r)
{
  if (r.width > 0 && r.height > 0)
    {
      self->edit_rect = easel_rect_union (self->edit_rect, r);
      cairo_rectangle_int_t cr = { r.x, r.y, r.width, r.height };
      if (self->dirty == NULL)
        self->dirty = cairo_region_create_rectangle (&cr);
      else
        cairo_region_union_rectangle (self->dirty, &cr);
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

static void
mark_all_dirty (EaselCanvas *self)
{
  mark_dirty (self, (EaselRect) { 0, 0, self->doc->width, self->doc->height });
}

/* The texture holds a private copy of the pixels (the document keeps changing
 * while GTK may still be rendering an older frame). Passing the previous
 * texture and the changed region lets GSK upload just that region to the GPU
 * instead of the whole image. */
static GdkTexture *
build_texture (EaselCanvas *self)
{
  const EaselDocument *d = self->doc;
  gsize stride = (gsize) d->width * 4;
  g_autoptr (GBytes) bytes = g_bytes_new (d->pixels, stride * (gsize) d->height);
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
  int size = (pixels * self->zoom + scale - 1) / scale;

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

/* Tools that show what they are about to do under the pointer. */
static gboolean
wants_hover_overlay (EaselTool tool)
{
  return tool == EASEL_TOOL_PENCIL || tool == EASEL_TOOL_BRUSH || tool == EASEL_TOOL_ERASER;
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
  GdkRGBA color = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f,
                    (c & 0xFF) / 255.0f, 0.65f };

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

static void
easel_canvas_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  EaselCanvas *self = EASEL_CANVAS (widget);
  double s = logical_per_pixel (self);

  refresh_texture (self);

  graphene_rect_t bounds = GRAPHENE_RECT_INIT (0, 0,
                                               (float) (self->doc->width * s),
                                               (float) (self->doc->height * s));
  gtk_snapshot_append_scaled_texture (snapshot, self->texture,
                                      GSK_SCALING_FILTER_NEAREST, &bounds);

  gtk_snapshot_push_clip (snapshot, &bounds);
  draw_brush_preview (self, snapshot, s);
  draw_eraser_outline (self, snapshot, s);
  gtk_snapshot_pop (snapshot);
}

static uint32_t
sample (EaselCanvas *self, int x, int y)
{
  /* Picking outside the image yields white, as classic Paint does. */
  if (x < 0 || y < 0 || x >= self->doc->width || y >= self->doc->height)
    return EASEL_RGB (255, 255, 255);
  return easel_document_get_pixel (self->doc, x, y);
}

/* Applies the freehand tool (pencil, brush or eraser) along a segment. A
 * zero-length segment is a single stamp. */
static EaselRect
apply_stroke (EaselCanvas *self, int x0, int y0, int x1, int y1)
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
        /* Left button: erase to the background colour. Right button: "color
         * eraser", replace only the foreground colour with the background. */
        EaselEraser eraser = {
          self->tool_size,
          self->background,
          self->button == GDK_BUTTON_SECONDARY,
          self->foreground,
        };
        return easel_raster_erase_stroke (self->doc, x0, y0, x1, y1, &eraser);
      }
    default:
      return (EaselRect) { 0, 0, 0, 0 };
    }
}

static void restore_shape_area (EaselCanvas *self);

/* Records the finished edit (whose "before" picture is `base`) for undo. */
static void
push_undo (EaselCanvas *self, const uint32_t *base)
{
  if (base != NULL && easel_history_push (self->history, self->doc, base, self->edit_rect))
    g_signal_emit (self, signals[SIGNAL_HISTORY_CHANGED], 0);
  self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
}

static void
begin_edit (EaselCanvas *self)
{
  g_free (self->edit_base);
  self->edit_base = g_memdup2 (self->doc->pixels, (gsize) self->doc->width * self->doc->height * 4);
  self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
}

static void
finish_edit (EaselCanvas *self)
{
  if (self->edit_base == NULL)
    return;
  push_undo (self, self->edit_base);
  g_clear_pointer (&self->edit_base, g_free);
}

static void
end_seq (EaselCanvas *self)
{
  self->seq_active = FALSE;
  self->seq_stage = 0;
  self->seq_n = 0;
  self->shape_prev = (EaselRect) { 0, 0, 0, 0 };
  g_clear_pointer (&self->backup, g_free);
}

/* Redraws the polygon or curve in progress from the saved pixels. */
static void
seq_render (EaselCanvas *self, gboolean closed, gboolean rubber, int rx, int ry)
{
  uint32_t outline = self->stroke_color;
  uint32_t fill = self->button == GDK_BUTTON_PRIMARY ? self->background : self->foreground;
  EaselRect now;

  restore_shape_area (self);
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

  mark_dirty (self, easel_rect_union (self->shape_prev, now));
  self->shape_prev = now;
}

static void
cancel_seq (EaselCanvas *self)
{
  if (!self->seq_active)
    return;
  restore_shape_area (self);
  mark_dirty (self, self->shape_prev);
  end_seq (self);
  self->drawing = FALSE;
}

/* Keeps what is on screen (without the rubber-band segment) as the result. */
static void
commit_seq (EaselCanvas *self, gboolean closed)
{
  if (!self->seq_active)
    return;
  seq_render (self, closed, FALSE, 0, 0);
  push_undo (self, self->backup);
  end_seq (self);
  self->drawing = FALSE;
}

static gboolean
spray_tick (gpointer data)
{
  EaselCanvas *self = data;
  int d = self->tool_size;

  mark_dirty (self, easel_raster_spray (self->doc, self->last_x, self->last_y, d,
                                        self->stroke_color, MAX (2, d * d / 8), &self->spray_rng));
  return G_SOURCE_CONTINUE;
}

static gboolean
is_shape_tool (EaselTool tool)
{
  return tool == EASEL_TOOL_LINE || tool == EASEL_TOOL_RECTANGLE
         || tool == EASEL_TOOL_ELLIPSE || tool == EASEL_TOOL_ROUNDED_RECTANGLE;
}

/* Puts back the pixels the previewed shape covered. */
static void
restore_shape_area (EaselCanvas *self)
{
  EaselRect r = self->shape_prev;

  for (int y = r.y; y < r.y + r.height; y++)
    memcpy (&self->doc->pixels[(size_t) y * self->doc->width + r.x],
            &self->backup[(size_t) y * self->doc->width + r.x], (size_t) r.width * 4);
}

/* Redraws the live shape from the drag start to image pixel (x, y). With
 * Shift held lines snap to 45 degree steps and boxes become squares/circles. */
static void
update_shape (EaselCanvas *self, int x, int y, gboolean constrain)
{
  int sx = self->start_ix, sy = self->start_iy;
  int dx = x - sx, dy = y - sy;
  int ax = abs (dx), ay = abs (dy);

  if (constrain)
    {
      if (self->tool == EASEL_TOOL_LINE)
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

  /* Left button: border in foreground, fill in background. Right: swapped. */
  uint32_t outline = self->stroke_color;
  uint32_t fill = self->button == GDK_BUTTON_PRIMARY ? self->background : self->foreground;
  EaselRect now;

  restore_shape_area (self);
  switch (self->tool)
    {
    case EASEL_TOOL_LINE:
      now = easel_raster_line (self->doc, sx, sy, x, y, self->line_width, outline);
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

  mark_dirty (self, easel_rect_union (self->shape_prev, now));
  self->shape_prev = now;
}

static void
end_shape_drag (EaselCanvas *self, gboolean keep)
{
  if (!keep)
    {
      restore_shape_area (self);
      mark_dirty (self, self->shape_prev);
      self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
    }
  else
    push_undo (self, self->backup);
  self->shape_prev = (EaselRect) { 0, 0, 0, 0 };
  g_clear_pointer (&self->backup, g_free);
}

static gboolean
shift_held (GtkGestureDrag *gesture)
{
  GdkModifierType state =
    gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
  return (state & GDK_SHIFT_MASK) != 0;
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
    case EASEL_TOOL_MAGNIFIER:
      easel_canvas_set_zoom (self, self->zoom == 1 ? self->zoom_level : 1,
                             self->last_x, self->last_y);
      break;

    case EASEL_TOOL_AIRBRUSH:
      self->drawing = TRUE;
      begin_edit (self);
      spray_tick (self);
      g_clear_handle_id (&self->spray_source, g_source_remove);
      self->spray_source = g_timeout_add (30, spray_tick, self);
      break;

    case EASEL_TOOL_POLYGON:
      {
        gint64 now = g_get_monotonic_time ();

        gtk_widget_grab_focus (GTK_WIDGET (self));
        if (!self->seq_active)
          {
            g_free (self->backup);
            self->backup = g_memdup2 (self->doc->pixels,
                                      (gsize) self->doc->width * self->doc->height * 4);
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
                commit_seq (self, TRUE); /* closes and fills */
                break;
              }
            if (self->seq_n < MAX_SEQ)
              self->seq[self->seq_n++] = (EaselPoint) { self->last_x, self->last_y };
          }
        self->last_press_us = now;
        self->drawing = TRUE;
        seq_render (self, FALSE, FALSE, 0, 0);
        break;
      }

    case EASEL_TOOL_CURVE:
      {
        EaselPoint here = { self->last_x, self->last_y };

        gtk_widget_grab_focus (GTK_WIDGET (self));
        if (!self->seq_active)
          {
            g_free (self->backup);
            self->backup = g_memdup2 (self->doc->pixels,
                                      (gsize) self->doc->width * self->doc->height * 4);
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
        seq_render (self, FALSE, FALSE, 0, 0);
        break;
      }

    case EASEL_TOOL_FILL:
      begin_edit (self);
      mark_dirty (self, easel_raster_flood_fill (self->doc, self->last_x, self->last_y,
                                                 self->stroke_color));
      finish_edit (self);
      break;

    case EASEL_TOOL_PICK_COLOR:
      self->picked = sample (self, self->last_x, self->last_y);
      self->drawing = TRUE; /* resolved on release */
      break;

    case EASEL_TOOL_LINE:
    case EASEL_TOOL_RECTANGLE:
    case EASEL_TOOL_ELLIPSE:
    case EASEL_TOOL_ROUNDED_RECTANGLE:
      self->drawing = TRUE;
      self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
      g_free (self->backup);
      self->backup = g_memdup2 (self->doc->pixels,
                                (gsize) self->doc->width * self->doc->height * 4);
      self->shape_prev = (EaselRect) { 0, 0, 0, 0 };
      break;

    case EASEL_TOOL_PENCIL:
    case EASEL_TOOL_BRUSH:
    case EASEL_TOOL_ERASER:
      self->drawing = TRUE;
      begin_edit (self);
      mark_dirty (self, apply_stroke (self, self->last_x, self->last_y,
                                      self->last_x, self->last_y));
      break;

    default:
      break;
    }
}

static void
on_drag_update (GtkGestureDrag *gesture, double offset_x, double offset_y, EaselCanvas *self)
{
  int x, y;

  if (!self->drawing)
    return;

  to_image (self, self->start_x + offset_x, self->start_y + offset_y, &x, &y);
  if (x == self->last_x && y == self->last_y)
    return;

  if (self->tool == EASEL_TOOL_PICK_COLOR)
    self->picked = sample (self, x, y);
  else if (is_shape_tool (self->tool))
    update_shape (self, x, y, shift_held (gesture));
  else if (self->tool == EASEL_TOOL_POLYGON)
    {
      self->seq[self->seq_n - 1] = (EaselPoint) { x, y };
      seq_render (self, FALSE, FALSE, 0, 0);
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
      seq_render (self, FALSE, FALSE, 0, 0);
    }
  else if (self->tool == EASEL_TOOL_AIRBRUSH)
    ; /* the timer sprays at last_x/last_y */
  else
    /* Join consecutive samples with a line so fast strokes have no gaps. */
    mark_dirty (self, apply_stroke (self, self->last_x, self->last_y, x, y));

  self->last_x = x;
  self->last_y = y;
}

static void
on_drag_end (GtkGestureDrag *gesture, double offset_x, double offset_y, EaselCanvas *self)
{
  if (!self->drawing)
    return;
  self->drawing = FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self)); /* hover preview comes back */

  if (self->tool == EASEL_TOOL_AIRBRUSH)
    {
      g_clear_handle_id (&self->spray_source, g_source_remove);
      finish_edit (self);
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
          seq_render (self, FALSE, FALSE, 0, 0);
        }
      return;
    }

  if (self->tool == EASEL_TOOL_CURVE)
    {
      if (self->seq_stage == 0)
        {
          if (self->seq[1].x == self->seq[0].x && self->seq[1].y == self->seq[0].y)
            cancel_seq (self);
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
              commit_seq (self, FALSE);
            }
        }
      else
        {
          if (!self->seq_moved)
            self->seq[3] = self->seq[2];
          commit_seq (self, FALSE);
        }
      return;
    }

  if (is_shape_tool (self->tool))
    {
      int x, y;
      to_image (self, self->start_x + offset_x, self->start_y + offset_y, &x, &y);
      /* A plain click (no movement) draws nothing. */
      if (x == self->start_ix && y == self->start_iy)
        end_shape_drag (self, FALSE);
      else
        {
          update_shape (self, x, y, shift_held (gesture));
          end_shape_drag (self, TRUE);
        }
      return;
    }

  if (self->tool == EASEL_TOOL_PICK_COLOR)
    {
      int x, y;
      to_image (self, self->start_x + offset_x, self->start_y + offset_y, &x, &y);
      self->picked = sample (self, x, y);
      g_signal_emit (self, signals[SIGNAL_COLOR_PICKED], 0,
                     (guint) self->picked, self->button == GDK_BUTTON_PRIMARY);
    }

  finish_edit (self); /* pencil, brush, eraser */
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller, guint keyval, guint keycode,
                GdkModifierType state, EaselCanvas *self)
{
  if (keyval != GDK_KEY_Escape)
    return FALSE;

  if (self->seq_active)
    cancel_seq (self);
  else if (self->drawing && is_shape_tool (self->tool))
    {
      end_shape_drag (self, FALSE);
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
  to_image (self, x, y, &self->hover_x, &self->hover_y);
  g_signal_emit (self, signals[SIGNAL_POINTER_MOVED], 0, self->hover_x, self->hover_y);
  if (self->seq_active && !self->drawing && self->tool == EASEL_TOOL_POLYGON)
    seq_render (self, FALSE, TRUE, self->hover_x, self->hover_y);
  if (wants_hover_overlay (self->tool))
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_leave (GtkEventControllerMotion *controller, EaselCanvas *self)
{
  self->hover_x = self->hover_y = -1;
  g_signal_emit (self, signals[SIGNAL_POINTER_MOVED], 0, -1, -1);
  if (wants_hover_overlay (self->tool))
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
  g_clear_object (&self->texture);
  g_clear_pointer (&self->dirty, cairo_region_destroy);
  g_clear_pointer (&self->backup, g_free);
  g_clear_handle_id (&self->spray_source, g_source_remove);
  g_clear_pointer (&self->edit_base, g_free);
  g_clear_pointer (&self->history, easel_history_free);
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

  signals[SIGNAL_POINTER_MOVED] =
    g_signal_new ("pointer-moved", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
  signals[SIGNAL_HISTORY_CHANGED] =
    g_signal_new ("history-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_COLOR_PICKED] =
    g_signal_new ("color-picked", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_BOOLEAN);
}

static void
easel_canvas_init (EaselCanvas *self)
{
  self->zoom = 1;
  self->foreground = EASEL_RGB (0, 0, 0);
  self->background = EASEL_RGB (255, 255, 255);
  self->tool = EASEL_TOOL_PENCIL;
  self->tool_size = 1;
  self->brush_shape = EASEL_BRUSH_CIRCLE;
  self->line_width = 1;
  self->zoom_level = 2;
  self->history = easel_history_new ((size_t) 512 << 20);
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
}

void
easel_canvas_set_background (EaselCanvas *self, uint32_t argb)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->background = argb;
}

void
easel_canvas_set_tool (EaselCanvas *self, EaselTool tool)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (tool != self->tool)
    commit_seq (self, FALSE);
  self->tool = tool;
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
    end_seq (self); /* the picture changed under it */
  mark_all_dirty (self);
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
      double s = logical_per_pixel (self);
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
  zoom = CLAMP (zoom, 1, 8);
  if (zoom == self->zoom)
    return;

  GtkWidget *sw = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_SCROLLED_WINDOW);
  if (ix < 0 && sw != NULL)
    {
      double s = logical_per_pixel (self);
      GtkAdjustment *h = gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (sw));
      GtkAdjustment *v = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (sw));
      ix = (int) ((gtk_adjustment_get_value (h) + gtk_adjustment_get_page_size (h) / 2) / s);
      iy = (int) ((gtk_adjustment_get_value (v) + gtk_adjustment_get_page_size (v) / 2) / s);
      ix = CLAMP (ix, 0, self->doc->width - 1);
      iy = CLAMP (iy, 0, self->doc->height - 1);
    }

  self->zoom = zoom;
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

  if (self->seq_active) /* Undo during a polygon or curve abandons it */
    {
      cancel_seq (self);
      return TRUE;
    }
  if (self->drawing || !easel_history_undo (self->history, self->doc, &r))
    return FALSE;
  mark_dirty (self, r);
  self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
  g_signal_emit (self, signals[SIGNAL_HISTORY_CHANGED], 0);
  return TRUE;
}

gboolean
easel_canvas_redo (EaselCanvas *self)
{
  g_return_val_if_fail (EASEL_IS_CANVAS (self), FALSE);
  EaselRect r;

  if (self->drawing || self->seq_active || !easel_history_redo (self->history, self->doc, &r))
    return FALSE;
  mark_dirty (self, r);
  self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
  g_signal_emit (self, signals[SIGNAL_HISTORY_CHANGED], 0);
  return TRUE;
}

/* Fills the whole image (Image > Clear Image) as one undoable step. */
void
easel_canvas_clear (EaselCanvas *self, uint32_t argb)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  commit_seq (self, FALSE);
  begin_edit (self);
  easel_document_fill (self->doc, argb);
  mark_all_dirty (self);
  finish_edit (self);
}
