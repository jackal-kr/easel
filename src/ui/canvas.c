#include "ui/canvas.h"

#include <math.h>
#include <string.h>

#include "core/clip.h"
#include "core/history.h"
#include "ui/cursors.h"

#define HANDLE_PAD  8  /* logical px reserved right/below the picture for resize handles */
#define HANDLE_SIZE 5

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

  /* Resize handles: 0 none, 1 right edge, 2 bottom edge, 3 corner */
  gboolean show_grid;
  int      handle_hover;
  int      resize_handle;   /* handle being dragged, 0 when not resizing */
  int      resize_w, resize_h;

  /* Selection. The picture is only touched when the selection is first
   * moved ("lifted": its pixels go to sel_clip and a background-coloured
   * hole is left) and when it is committed (the clip is drawn back down). */
  gboolean    sel_active;
  EaselRect   sel_rect;      /* current position and size, image pixels */
  EaselClip  *sel_clip;      /* lifted contents, NULL until the first move */
  uint32_t   *sel_base;      /* picture before the lift, for undo */
  EaselRect   sel_edit;      /* area touched by the lift */
  GdkTexture *sel_texture;   /* sel_clip as shown on screen */
  gboolean    sel_creating, sel_moving;
  int         sel_grab_x, sel_grab_y;
  uint8_t    *sel_mask;      /* free-form shape inside sel_rect, NULL for a rectangle */
  GArray     *sel_path;      /* free-form outline (EaselPoint), NULL for a rectangle */
  int         sel_path_x, sel_path_y; /* sel_rect origin when the outline was made */
  EaselClip  *sel_src;       /* contents when a stretch began */
  EaselRect   sel_src_rect;
  int         sel_stretch;   /* handle being dragged (1..8), 0 none */
  gboolean    sel_opaque;    /* Draw Opaque: background-coloured pixels are kept */

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

enum { SIGNAL_POINTER_MOVED, SIGNAL_COLOR_PICKED, SIGNAL_HISTORY_CHANGED, SIGNAL_RESIZE_PREVIEW, SIGNAL_ZOOM_CHANGED, SIGNAL_SELECTION_CHANGED, N_SIGNALS };
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
  int size = (pixels * self->zoom + scale - 1) / scale + HANDLE_PAD;

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

/* Centre of each resize handle, in widget coordinates. */
static void
handle_center (EaselCanvas *self, int handle, double *x, double *y)
{
  double s = logical_per_pixel (self);
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
      double s = logical_per_pixel (self);
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
  if (!self->show_grid || self->zoom < 4)
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

static void sel_handle_pos (const EaselRect *r, int h, double s, double *wx, double *wy);

/* Floating contents plus a dashed outline (white under black, so it shows on
 * any colour). */
static void
draw_selection (EaselCanvas *self, GtkSnapshot *snapshot, double s)
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
        sel_handle_pos (&r, h, s, &cx, &cy);
        graphene_rect_t halo = GRAPHENE_RECT_INIT ((float) cx - 3.5f, (float) cy - 3.5f, 7, 7);
        graphene_rect_t dot = GRAPHENE_RECT_INIT ((float) cx - 2.5f, (float) cy - 2.5f, 5, 5);
        gtk_snapshot_append_color (snapshot, &white, &halo);
        gtk_snapshot_append_color (snapshot, &black, &dot);
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
                                      GSK_SCALING_FILTER_NEAREST, &bounds);

  gtk_snapshot_push_clip (snapshot, &bounds);
  draw_brush_preview (self, snapshot, s);
  draw_eraser_outline (self, snapshot, s);
  draw_grid (self, snapshot, &bounds, s);
  draw_selection (self, snapshot, s);
  gtk_snapshot_pop (snapshot);

  draw_handles (self, snapshot);
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

/* ---- Selection ------------------------------------------------------- */

static GdkTexture *
texture_from_clip (const EaselClip *clip, gboolean opaque, uint32_t key, int checker)
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

static void
sel_refresh_texture (EaselCanvas *self)
{
  g_clear_object (&self->sel_texture);
  if (self->sel_clip != NULL)
    self->sel_texture = texture_from_clip (self->sel_clip, self->sel_opaque, self->background,
                                           MAX (1, 8 / self->zoom));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
sel_notify (EaselCanvas *self)
{
  g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
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
static void
sel_lift (EaselCanvas *self, gboolean copy)
{
  if (self->sel_clip != NULL || !self->sel_active)
    return;

  self->sel_clip = easel_clip_copy_from (self->doc, self->sel_rect, self->sel_mask);
  if (self->sel_clip == NULL)
    return;
  g_free (self->sel_base);
  self->sel_base = g_memdup2 (self->doc->pixels, (gsize) self->doc->width * self->doc->height * 4);
  self->sel_edit = (EaselRect) { 0, 0, 0, 0 };
  if (!copy)
    {
      self->sel_edit = easel_clip_erase (self->doc, self->sel_rect, self->sel_mask, hole_color (self));
      mark_dirty (self, self->sel_edit);
    }
  sel_refresh_texture (self);
}

static void
sel_discard (EaselCanvas *self)
{
  g_clear_pointer (&self->sel_clip, easel_clip_free);
  g_clear_pointer (&self->sel_base, g_free);
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
static void
sel_commit (EaselCanvas *self)
{
  if (!self->sel_active && !self->sel_creating)
    return;

  if (self->sel_clip != NULL)
    {
      EaselRect landed = easel_clip_blit (self->doc, self->sel_clip, self->sel_rect.x, self->sel_rect.y,
                                          !self->sel_opaque, self->background);
      self->edit_rect = easel_rect_union (self->sel_edit, landed);
      mark_dirty (self, landed);
      push_undo (self, self->sel_base);
    }
  sel_discard (self);
  sel_notify (self);
}

/* Drag corners to an image-clamped rectangle. */
static EaselRect
sel_rect_between (EaselCanvas *self, int x0, int y0, int x1, int y1)
{
  int left = CLAMP (MIN (x0, x1), 0, self->doc->width - 1);
  int top = CLAMP (MIN (y0, y1), 0, self->doc->height - 1);
  int right = CLAMP (MAX (x0, x1), 0, self->doc->width - 1);
  int bottom = CLAMP (MAX (y0, y1), 0, self->doc->height - 1);

  return (EaselRect) { left, top, right - left + 1, bottom - top + 1 };
}

/* Stretch handles: 1 NW, 2 N, 3 NE, 4 W, 5 E, 6 SW, 7 S, 8 SE. */
static void
sel_handle_pos (const EaselRect *r, int h, double s, double *wx, double *wy)
{
  int col = (h == 1 || h == 4 || h == 6) ? 0 : (h == 2 || h == 7) ? 1 : 2;
  int row = (h <= 3) ? 0 : (h <= 5) ? 1 : 2;

  *wx = (r->x + r->width * col / 2.0) * s;
  *wy = (r->y + r->height * row / 2.0) * s;
}

static int
sel_handle_at (EaselCanvas *self, double wx, double wy)
{
  if (!self->sel_active || self->sel_creating)
    return 0;
  double s = logical_per_pixel (self);

  for (int h = 1; h <= 8; h++)
    {
      double cx, cy;
      sel_handle_pos (&self->sel_rect, h, s, &cx, &cy);
      if (fabs (wx - cx) <= 5 && fabs (wy - cy) <= 5)
        return h;
    }
  return 0;
}

static void
sel_stretch_to (EaselCanvas *self, int x, int y)
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
  sel_refresh_texture (self);
}

static gboolean
is_select_tool (EaselTool tool)
{
  return tool == EASEL_TOOL_SELECT || tool == EASEL_TOOL_FREE_FORM_SELECT;
}

static gboolean
sel_contains (EaselCanvas *self, int x, int y)
{
  return self->sel_active && x >= self->sel_rect.x && y >= self->sel_rect.y
         && x < self->sel_rect.x + self->sel_rect.width && y < self->sel_rect.y + self->sel_rect.height;
}

static gboolean
ctrl_held (GtkGestureDrag *gesture)
{
  GdkModifierType state =
    gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
  return (state & GDK_CONTROL_MASK) != 0;
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

  if (button == GDK_BUTTON_PRIMARY)
    {
      int handle = handle_at (self, x, y);
      if (handle != 0)
        {
          commit_seq (self, FALSE);
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
    case EASEL_TOOL_SELECT:
    case EASEL_TOOL_FREE_FORM_SELECT:
      self->drawing = TRUE;
      if ((self->sel_stretch = sel_handle_at (self, x, y)) != 0)
        {
          sel_lift (self, FALSE);
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
      else if (sel_contains (self, self->last_x, self->last_y))
        {
          sel_lift (self, ctrl_held (gesture));
          self->sel_moving = TRUE;
          self->sel_grab_x = self->last_x - self->sel_rect.x;
          self->sel_grab_y = self->last_y - self->sel_rect.y;
        }
      else
        {
          sel_commit (self);
          self->sel_creating = TRUE;
          self->sel_rect = sel_rect_between (self, self->last_x, self->last_y, self->last_x, self->last_y);
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

  if (self->resize_handle != 0)
    {
      double s = logical_per_pixel (self);
      int nx = (int) lround ((self->start_x + offset_x) / s);
      int ny = (int) lround ((self->start_y + offset_y) / s);

      /* The handle sits just past the edge, so measure from the start point. */
      nx = self->doc->width + (nx - (int) lround (self->start_x / s));
      ny = self->doc->height + (ny - (int) lround (self->start_y / s));
      if (self->resize_handle != 2)
        self->resize_w = CLAMP (nx, 1, 16384);
      if (self->resize_handle != 1)
        self->resize_h = CLAMP (ny, 1, 16384);
      g_signal_emit (self, signals[SIGNAL_RESIZE_PREVIEW], 0, self->resize_w, self->resize_h);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

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
  else if (is_select_tool (self->tool))
    {
      if (self->sel_stretch != 0)
        sel_stretch_to (self, x, y);
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
        self->sel_rect = sel_rect_between (self, self->start_ix, self->start_iy, x, y);
      gtk_widget_queue_draw (GTK_WIDGET (self));
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
  if (self->resize_handle != 0)
    {
      self->resize_handle = 0;
      self->handle_hover = -1; /* next motion re-picks the cursor */
      gtk_widget_set_cursor (GTK_WIDGET (self), easel_cursor_for_tool (self->tool));
      g_signal_emit (self, signals[SIGNAL_RESIZE_PREVIEW], 0, -1, -1);
      if (self->resize_w != self->doc->width || self->resize_h != self->doc->height)
        easel_canvas_resize (self, self->resize_w, self->resize_h);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    }

  if (!self->drawing)
    return;
  self->drawing = FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self)); /* hover preview comes back */

  if (is_select_tool (self->tool))
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
          sel_notify (self);
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
          sel_notify (self);
        }
      self->sel_moving = FALSE;
      self->sel_stretch = 0;
      g_clear_pointer (&self->sel_src, easel_clip_free);
      return;
    }

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
  if (keyval == GDK_KEY_Delete && self->sel_active)
    {
      easel_canvas_clear_selection (self);
      return TRUE;
    }

  if (keyval != GDK_KEY_Escape)
    return FALSE;

  if (self->sel_active || self->sel_creating)
    sel_commit (self);
  else if (self->seq_active)
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
  /* Over a selection the pointer offers to move it. */
  if (is_select_tool (self->tool) && self->resize_handle == 0 && self->handle_hover == 0)
    {
      int ix, iy;
      static const char *const stretch[] = { NULL, "nwse-resize", "ns-resize", "nesw-resize", "ew-resize",
                                             "ew-resize", "nesw-resize", "ns-resize", "nwse-resize" };
      int sh = self->sel_stretch != 0 ? self->sel_stretch : sel_handle_at (self, x, y);
      to_image (self, x, y, &ix, &iy);
      if (sh != 0)
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self), stretch[sh]);
      else if (sel_contains (self, ix, iy))
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "move");
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
  g_clear_pointer (&self->sel_clip, easel_clip_free);
  g_clear_pointer (&self->sel_base, g_free);
  g_clear_object (&self->sel_texture);
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
  signals[SIGNAL_SELECTION_CHANGED] =
    g_signal_new ("selection-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_ZOOM_CHANGED] =
    g_signal_new ("zoom-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_RESIZE_PREVIEW] =
    g_signal_new ("resize-preview", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
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
  self->sel_opaque = TRUE;
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
  if (self->sel_clip != NULL && !self->sel_opaque)
    sel_refresh_texture (self);
}

void
easel_canvas_set_tool (EaselCanvas *self, EaselTool tool)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (tool != self->tool)
    {
      sel_commit (self);
      commit_seq (self, FALSE);
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
  if (self->sel_clip != NULL && !self->sel_opaque)
    sel_refresh_texture (self); /* checkerboard cell size follows the zoom */
  g_signal_emit (self, signals[SIGNAL_ZOOM_CHANGED], 0);
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

  sel_commit (self);
  if (self->seq_active) /* Undo during a polygon or curve abandons it */
    {
      cancel_seq (self);
      return TRUE;
    }
  if (self->drawing || !easel_history_undo (self->history, self->doc, &r))
    return FALSE;
  gtk_widget_queue_resize (GTK_WIDGET (self)); /* the undone step may have changed the size */
  mark_all_dirty (self);
  self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
  g_signal_emit (self, signals[SIGNAL_HISTORY_CHANGED], 0);
  return TRUE;
}

gboolean
easel_canvas_redo (EaselCanvas *self)
{
  g_return_val_if_fail (EASEL_IS_CANVAS (self), FALSE);
  EaselRect r;

  sel_commit (self);
  if (self->drawing || self->seq_active || !easel_history_redo (self->history, self->doc, &r))
    return FALSE;
  gtk_widget_queue_resize (GTK_WIDGET (self)); /* the undone step may have changed the size */
  mark_all_dirty (self);
  self->edit_rect = (EaselRect) { 0, 0, 0, 0 };
  g_signal_emit (self, signals[SIGNAL_HISTORY_CHANGED], 0);
  return TRUE;
}

/* Fills the whole image (Image > Clear Image) as one undoable step. */
void
easel_canvas_clear (EaselCanvas *self, uint32_t argb)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  sel_commit (self);
  commit_seq (self, FALSE);
  begin_edit (self);
  easel_document_fill (self->doc, argb);
  mark_all_dirty (self);
  finish_edit (self);
}

/* Replaces the picture (File > Open, New). Takes ownership of `doc`; undo
 * history and any tool in progress are discarded. */
void
easel_canvas_set_document (EaselCanvas *self, EaselDocument *doc)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  g_return_if_fail (doc != NULL);

  g_clear_handle_id (&self->spray_source, g_source_remove);
  sel_discard (self);
  sel_notify (self);
  g_clear_pointer (&self->backup, g_free);
  g_clear_pointer (&self->edit_base, g_free);
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
  self->zoom = 1;
  g_signal_emit (self, signals[SIGNAL_ZOOM_CHANGED], 0);
  gtk_widget_queue_resize (GTK_WIDGET (self));
  mark_all_dirty (self);
  g_signal_emit (self, signals[SIGNAL_HISTORY_CHANGED], 0);
}

/* Changes the picture size (crop or extend with the background colour) as one
 * undoable step. */
gboolean
easel_canvas_resize (EaselCanvas *self, int width, int height)
{
  g_return_val_if_fail (EASEL_IS_CANVAS (self), FALSE);

  sel_commit (self);
  if (width == self->doc->width && height == self->doc->height)
    return TRUE;
  commit_seq (self, FALSE);

  int old_w = self->doc->width, old_h = self->doc->height;
  g_autofree uint32_t *old = g_memdup2 (self->doc->pixels, (gsize) old_w * old_h * 4);

  if (!easel_document_resize (self->doc, width, height, self->background))
    return FALSE;

  easel_history_push_full (self->history, old, old_w, old_h, self->doc);
  gtk_widget_queue_resize (GTK_WIDGET (self));
  mark_all_dirty (self);
  g_signal_emit (self, signals[SIGNAL_HISTORY_CHANGED], 0);
  return TRUE;
}

void
easel_canvas_set_show_grid (EaselCanvas *self, gboolean show)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->show_grid = show;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ---- Selection API --------------------------------------------------- */

gboolean
easel_canvas_has_selection (EaselCanvas *self)
{
  g_return_val_if_fail (EASEL_IS_CANVAS (self), FALSE);
  return self->sel_active;
}

/* Puts any floating selection down; call before saving or other whole-picture work. */
void
easel_canvas_flush (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  sel_commit (self);
  commit_seq (self, FALSE);
}

void
easel_canvas_set_opaque (EaselCanvas *self, gboolean opaque)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->sel_opaque = opaque;
  if (self->sel_clip != NULL)
    sel_refresh_texture (self);
}

void
easel_canvas_select_all (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  sel_commit (self);
  self->sel_rect = (EaselRect) { 0, 0, self->doc->width, self->doc->height };
  self->sel_active = TRUE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  sel_notify (self);
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
  g_autoptr (GdkTexture) texture = texture_from_clip (clip, TRUE, 0, 0);
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
      push_undo (self, self->sel_base);
      sel_discard (self);
    }
  else
    {
      begin_edit (self);
      mark_dirty (self, easel_clip_erase (self->doc, self->sel_rect, self->sel_mask, hole_color (self)));
      finish_edit (self);
      sel_discard (self);
    }
  sel_notify (self);
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

  sel_commit (self);
  commit_seq (self, FALSE);
  if (clip->width > self->doc->width || clip->height > self->doc->height)
    easel_canvas_resize (self, MAX (clip->width, self->doc->width), MAX (clip->height, self->doc->height));

  int x = 0, y = 0;
  GtkWidget *sw = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_SCROLLED_WINDOW);
  if (sw != NULL)
    {
      double s = logical_per_pixel (self);
      x = (int) (gtk_adjustment_get_value (gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (sw))) / s);
      y = (int) (gtk_adjustment_get_value (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (sw))) / s);
    }

  g_free (self->sel_base);
  self->sel_base = g_memdup2 (self->doc->pixels, (gsize) self->doc->width * self->doc->height * 4);
  self->sel_edit = (EaselRect) { 0, 0, 0, 0 };
  self->sel_clip = clip;
  self->sel_rect = (EaselRect) { x, y, clip->width, clip->height };
  self->sel_active = TRUE;
  sel_refresh_texture (self);
  sel_notify (self);
}
