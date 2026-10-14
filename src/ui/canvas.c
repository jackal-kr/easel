#include "ui/canvas.h"

#include <math.h>
#include <string.h>
#include <pango/pangocairo.h>

#include "core/clip.h"
#include "core/history.h"
#include "core/transform.h"
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

  gint64   paint_us; /* time the last snapshot took (CPU side) */
  int      zoom; /* magnification in percent, 100 = actual size */
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
  /* Text tool */
  gboolean    text_active, text_creating;
  EaselRect   text_rect;
  GString    *text_buf;
  int         text_cursor;   /* byte offset */
  char       *font;
  EaselClip  *text_clip;
  GdkTexture *text_texture;
  PangoLayout *text_layout;
  gboolean    text_opaque, text_smooth;
  gboolean    panning;
  int         gradient_style;
  EaselRect   grad_region;   /* what the gradient fills: the picture or the selection */
  gboolean    grad_masked;
  gboolean    grad_pending;  /* drawn but still adjustable: drag its end points */
  int         grad_x0, grad_y0, grad_x1, grad_y1;
  int         grad_drag;     /* end point being dragged: 1 start, 2 end, 0 none */
  guint       grad_button;
  double      pan_x0, pan_y0, pan_h0, pan_v0; /* pointer and scroll position at the start */
  GArray     *text_attr;     /* TextAttr per byte of text_buf */
  int         text_anchor;   /* other end of the text selection, -1 none */
  guint       text_typing;   /* style flags for newly typed text */
  guint       text_blink_id;
  gboolean    text_caret_on;
  int         text_drag;      /* 0 none, 1 moving the box, 2 resizing it */
  int         text_handle;    /* handle (1..8) while resizing */
  int         text_grab_x, text_grab_y;
  EaselRect   text_orig;
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

typedef struct { guint8 flags; uint32_t color; } TextAttr;

G_DEFINE_FINAL_TYPE (EaselCanvas, easel_canvas, GTK_TYPE_WIDGET)

enum { SIGNAL_POINTER_MOVED, SIGNAL_COLOR_PICKED, SIGNAL_HISTORY_CHANGED, SIGNAL_RESIZE_PREVIEW, SIGNAL_ZOOM_CHANGED, SIGNAL_SELECTION_CHANGED, SIGNAL_TEXT_STATE, N_SIGNALS };
static guint signals[N_SIGNALS];

/* One image pixel is one *device* pixel at zoom 1, even on HiDPI screens,
 * so the logical widget size is image size * zoom / scale factor. */
static double
logical_per_pixel (EaselCanvas *self)
{
  return self->zoom / 100.0 / gtk_widget_get_scale_factor (GTK_WIDGET (self));
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

static void text_commit (EaselCanvas *self);
static void grad_commit (EaselCanvas *self);
static void draw_gradient_guide (EaselCanvas *self, GtkSnapshot *snapshot, double s);
static void text_caret_restart (EaselCanvas *self);
static void draw_text (EaselCanvas *self, GtkSnapshot *snapshot, double s);
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
  draw_selection (self, snapshot, s);
  draw_text (self, snapshot, s);
  draw_gradient_guide (self, snapshot, s);
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
                                           MAX (1, 800 / self->zoom));
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
  grad_commit (self);
  text_commit (self);
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


/* ---- Text ------------------------------------------------------------ */

static gboolean
text_sel_range (EaselCanvas *self, int *a, int *b)
{
  if (!self->text_active || self->text_anchor < 0 || self->text_anchor == self->text_cursor)
    return FALSE;
  *a = MIN (self->text_anchor, self->text_cursor);
  *b = MAX (self->text_anchor, self->text_cursor);
  return TRUE;
}

/* Tells the options and the Edit menu what the caret or selection looks like. */
static void
text_emit_state (EaselCanvas *self)
{
  guint flags = self->text_typing;
  int a, b;

  if (text_sel_range (self, &a, &b))
    {
      flags = 0xF;
      for (int i = a; i < b; i++)
        flags &= g_array_index (self->text_attr, TextAttr, i).flags;
    }
  g_signal_emit (self, signals[SIGNAL_TEXT_STATE], 0, flags);
  g_signal_emit (self, signals[SIGNAL_SELECTION_CHANGED], 0);
}

static void
text_buf_insert (EaselCanvas *self, int pos, const char *str)
{
  int len = (int) strlen (str);
  TextAttr at = { (guint8) self->text_typing, self->foreground | 0xFF000000u };

  g_string_insert (self->text_buf, pos, str);
  for (int i = 0; i < len; i++)
    g_array_insert_val (self->text_attr, pos + i, at);
}

static void
text_buf_erase (EaselCanvas *self, int pos, int len)
{
  g_string_erase (self->text_buf, pos, len);
  g_array_remove_range (self->text_attr, pos, len);
}

static void
text_free_render (EaselCanvas *self)
{
  g_clear_pointer (&self->text_clip, easel_clip_free);
  g_clear_object (&self->text_texture);
  g_clear_object (&self->text_layout);
}

static uint32_t
text_rgb (uint32_t c)
{
  return c | 0xFF000000u;
}

/* Lays the text out and draws it, without antialiasing, into a clip the size
 * of the box. Opaque: the background fills the box. Transparent: only the
 * glyph pixels are set. The box grows downward to fit its text. */
static void
text_render (EaselCanvas *self)
{
  text_free_render (self);
  if (!self->text_active)
    return;

  PangoContext *ctx = pango_font_map_create_context (pango_cairo_font_map_get_default ());
  cairo_font_options_t *fo = cairo_font_options_create ();
  cairo_font_options_set_antialias (fo, self->text_smooth ? CAIRO_ANTIALIAS_GRAY : CAIRO_ANTIALIAS_NONE);
  cairo_font_options_set_hint_metrics (fo, self->text_smooth ? CAIRO_HINT_METRICS_OFF : CAIRO_HINT_METRICS_ON);
  cairo_font_options_set_hint_style (fo, self->text_smooth ? CAIRO_HINT_STYLE_SLIGHT : CAIRO_HINT_STYLE_FULL);
  pango_cairo_context_set_font_options (ctx, fo);
  pango_cairo_context_set_resolution (ctx, 96);
  cairo_font_options_destroy (fo);

  PangoLayout *layout = pango_layout_new (ctx);
  PangoFontDescription *desc = pango_font_description_from_string (self->font);
  pango_layout_set_font_description (layout, desc);
  pango_font_description_free (desc);
  pango_layout_set_width (layout, self->text_rect.width * PANGO_SCALE);
  pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_text (layout, self->text_buf->str, (int) self->text_buf->len);
  {
    PangoAttrList *attrs = pango_attr_list_new ();
    guint n = self->text_attr->len, i = 0;

    while (i < n)
      {
        TextAttr a = g_array_index (self->text_attr, TextAttr, i);
        guint j = i + 1;
        while (j < n)
          {
            TextAttr b = g_array_index (self->text_attr, TextAttr, j);
            if (b.flags != a.flags || b.color != a.color)
              break;
            j++;
          }
        PangoAttribute *at[5];
        int k = 0;
        at[k++] = pango_attr_foreground_new (((a.color >> 16) & 0xFF) * 257, ((a.color >> 8) & 0xFF) * 257,
                                             (a.color & 0xFF) * 257);
        if (a.flags & EASEL_TEXT_BOLD)
          at[k++] = pango_attr_weight_new (PANGO_WEIGHT_BOLD);
        if (a.flags & EASEL_TEXT_ITALIC)
          at[k++] = pango_attr_style_new (PANGO_STYLE_ITALIC);
        if (a.flags & EASEL_TEXT_UNDERLINE)
          at[k++] = pango_attr_underline_new (PANGO_UNDERLINE_SINGLE);
        if (a.flags & EASEL_TEXT_STRIKE)
          at[k++] = pango_attr_strikethrough_new (TRUE);
        for (int m = 0; m < k; m++)
          {
            at[m]->start_index = i;
            at[m]->end_index = j;
            pango_attr_list_insert (attrs, at[m]);
          }
        i = j;
      }
    pango_layout_set_attributes (layout, attrs);
    pango_attr_list_unref (attrs);
  }
  g_object_unref (ctx);

  int lw, lh;
  pango_layout_get_pixel_size (layout, &lw, &lh);
  if (lh > self->text_rect.height)
    self->text_rect.height = lh;

  int w = self->text_rect.width, h = self->text_rect.height;
  cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create (surf);
  cairo_set_antialias (cr, self->text_smooth ? CAIRO_ANTIALIAS_GRAY : CAIRO_ANTIALIAS_NONE);
  if (self->text_opaque)
    {
      uint32_t bg = text_rgb (self->background);
      cairo_set_source_rgb (cr, ((bg >> 16) & 0xFF) / 255.0, ((bg >> 8) & 0xFF) / 255.0, (bg & 0xFF) / 255.0);
      cairo_paint (cr);
    }
  uint32_t fg = text_rgb (self->foreground);
  cairo_set_source_rgb (cr, ((fg >> 16) & 0xFF) / 255.0, ((fg >> 8) & 0xFF) / 255.0, (fg & 0xFF) / 255.0);
  pango_cairo_show_layout (cr, layout);
  cairo_destroy (cr);
  cairo_surface_flush (surf);

  EaselClip *clip = easel_clip_new (w, h);
  if (clip != NULL)
    {
      const unsigned char *data = cairo_image_surface_get_data (surf);
      int stride = cairo_image_surface_get_stride (surf);

      for (int y = 0; y < h; y++)
        memcpy (clip->pixels + (gsize) y * w, data + (gsize) y * stride, (gsize) w * 4);
      self->text_clip = clip;
      self->text_texture = texture_from_clip (clip, TRUE, 0, 0);
    }
  cairo_surface_destroy (surf);
  self->text_layout = layout;
  text_caret_restart (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
text_blink_tick (gpointer data)
{
  EaselCanvas *self = data;

  self->text_caret_on = !self->text_caret_on;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  return G_SOURCE_CONTINUE;
}

static void
text_caret_restart (EaselCanvas *self)
{
  self->text_caret_on = TRUE;
  g_clear_handle_id (&self->text_blink_id, g_source_remove);
  if (self->text_active)
    self->text_blink_id = g_timeout_add (530, text_blink_tick, self);
}

static void
text_cancel (EaselCanvas *self)
{
  g_clear_handle_id (&self->text_blink_id, g_source_remove);
  self->text_drag = 0;
  g_clear_handle_id (&self->text_blink_id, g_source_remove);
  text_free_render (self);
  if (self->text_buf != NULL)
    g_string_truncate (self->text_buf, 0);
  if (self->text_attr != NULL)
    g_array_set_size (self->text_attr, 0);
  self->text_anchor = -1;
  self->text_active = self->text_creating = FALSE;
  self->text_cursor = 0;
  text_emit_state (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* Puts the text down as pixels: one undo step. */
static void
text_commit (EaselCanvas *self)
{
  if (!self->text_active && !self->text_creating)
    return;

  if (self->text_active && self->text_clip != NULL && self->text_buf->len > 0)
    {
      g_autofree uint32_t *base = g_memdup2 (self->doc->pixels, (gsize) self->doc->width * self->doc->height * 4);
      /* Source-over, so smoothed edges blend with what is underneath. */
      EaselRect box = self->text_rect, landed = { 0, 0, 0, 0 };
      for (int y = 0; y < self->text_clip->height; y++)
        for (int x = 0; x < self->text_clip->width; x++)
          {
            int dx = box.x + x, dy = box.y + y;
            uint32_t src = self->text_clip->pixels[(gsize) y * self->text_clip->width + x];
            uint32_t a = src >> 24;

            if (dx < 0 || dy < 0 || dx >= self->doc->width || dy >= self->doc->height || a == 0)
              continue;
            uint32_t *dst = &self->doc->pixels[(gsize) dy * self->doc->width + dx];
            if (a != 255)
              {
                uint32_t d = *dst, inv = 255 - a, out = 0;
                for (int sh = 0; sh <= 24; sh += 8)
                  {
                    uint32_t v = ((src >> sh) & 0xFF) + (((d >> sh) & 0xFF) * inv + 127) / 255;
                    out |= MIN (v, 255u) << sh;
                  }
                src = out;
              }
            *dst = src;
            landed = easel_rect_union (landed, (EaselRect) { dx, dy, 1, 1 });
          }
      self->edit_rect = landed;
      mark_dirty (self, landed);
      push_undo (self, base);
    }
  text_cancel (self);
}

static gboolean
text_delete_selection (EaselCanvas *self)
{
  int a, b;

  if (!text_sel_range (self, &a, &b))
    return FALSE;
  text_buf_erase (self, a, b - a);
  self->text_cursor = a;
  self->text_anchor = -1;
  return TRUE;
}

static void
text_insert (EaselCanvas *self, const char *utf8)
{
  text_delete_selection (self);
  text_buf_insert (self, self->text_cursor, utf8);
  self->text_cursor += (int) strlen (utf8);
  text_render (self);
  text_emit_state (self);
}

/* Moves the caret; with `extend` the old position becomes the selection's
 * other end. The typing style follows the character before the caret. */
static void
text_move_cursor (EaselCanvas *self, int index, gboolean extend)
{
  if (extend && self->text_anchor < 0)
    self->text_anchor = self->text_cursor;
  else if (!extend)
    self->text_anchor = -1;
  self->text_cursor = CLAMP (index, 0, (int) self->text_buf->len);
  if (!extend && self->text_attr->len > 0)
    {
      guint at = self->text_cursor > 0 ? (guint) self->text_cursor - 1 : 0;
      self->text_typing = g_array_index (self->text_attr, TextAttr, at).flags;
    }
  text_caret_restart (self);
  text_emit_state (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static int
text_index_at (EaselCanvas *self, int ix, int iy)
{
  int idx = 0, trailing = 0;

  if (self->text_layout == NULL)
    return 0;
  pango_layout_xy_to_index (self->text_layout, (ix - self->text_rect.x) * PANGO_SCALE,
                            (iy - self->text_rect.y) * PANGO_SCALE, &idx, &trailing);
  if (trailing > 0)
    idx = (int) (g_utf8_offset_to_pointer (self->text_buf->str + idx, trailing) - self->text_buf->str);
  return idx;
}

/* Bold, italic ... on the selection (or, with none, on what is typed next).
 * Returns nothing; the options follow through the text-state signal. */
static void
text_apply_style (EaselCanvas *self, guint flag, gboolean on)
{
  int a, b;

  if (self->text_active && text_sel_range (self, &a, &b))
    {
      for (int i = a; i < b; i++)
        {
          TextAttr *at = &g_array_index (self->text_attr, TextAttr, i);
          at->flags = on ? (at->flags | flag) : (at->flags & ~flag);
        }
      text_render (self);
    }
  else if (on)
    self->text_typing |= flag;
  else
    self->text_typing &= ~flag;
  if (self->text_active)
    text_emit_state (self);
}

/* The colour goes to the selected text, or to all of it with no selection. */
static void
text_apply_color (EaselCanvas *self, uint32_t color)
{
  int a = 0, b;

  if (!self->text_active)
    return;
  b = (int) self->text_attr->len;
  text_sel_range (self, &a, &b);
  for (int i = a; i < b; i++)
    g_array_index (self->text_attr, TextAttr, i).color = color | 0xFF000000u;
  text_render (self);
}

/* Returns TRUE if the key was consumed by the text box. */
static gboolean
text_key (EaselCanvas *self, guint keyval, GdkModifierType state)
{
  const char *str = self->text_buf->str;
  int len = (int) self->text_buf->len;
  gboolean shift = (state & GDK_SHIFT_MASK) != 0;

  if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK))
    return FALSE;

  switch (keyval)
    {
    case GDK_KEY_Escape:
      text_commit (self);
      return TRUE;
    case GDK_KEY_BackSpace:
      if (!text_delete_selection (self) && self->text_cursor > 0)
        {
          int prev = (int) (g_utf8_prev_char (str + self->text_cursor) - str);
          text_buf_erase (self, prev, self->text_cursor - prev);
          self->text_cursor = prev;
        }
      text_render (self);
      text_emit_state (self);
      return TRUE;
    case GDK_KEY_Delete:
      if (!text_delete_selection (self) && self->text_cursor < len)
        {
          int next = (int) (g_utf8_next_char (str + self->text_cursor) - str);
          text_buf_erase (self, self->text_cursor, next - self->text_cursor);
        }
      text_render (self);
      text_emit_state (self);
      return TRUE;
    case GDK_KEY_Left:
      if (self->text_cursor > 0)
        text_move_cursor (self, (int) (g_utf8_prev_char (str + self->text_cursor) - str), shift);
      return TRUE;
    case GDK_KEY_Right:
      if (self->text_cursor < len)
        text_move_cursor (self, (int) (g_utf8_next_char (str + self->text_cursor) - str), shift);
      return TRUE;
    case GDK_KEY_Up:
    case GDK_KEY_Down:
      if (self->text_layout != NULL)
        {
          int line, x;
          pango_layout_index_to_line_x (self->text_layout, self->text_cursor, FALSE, &line, &x);
          line += keyval == GDK_KEY_Up ? -1 : 1;
          if (line >= 0 && line < pango_layout_get_line_count (self->text_layout))
            {
              int idx, trailing;
              pango_layout_line_x_to_index (pango_layout_get_line_readonly (self->text_layout, line), x,
                                            &idx, &trailing);
              text_move_cursor (self, idx, shift);
            }
        }
      return TRUE;
    case GDK_KEY_Home:
    case GDK_KEY_End:
      if (self->text_layout != NULL)
        {
          int line, x;
          PangoLayoutLine *l;
          pango_layout_index_to_line_x (self->text_layout, self->text_cursor, FALSE, &line, &x);
          l = pango_layout_get_line_readonly (self->text_layout, line);
          text_move_cursor (self, keyval == GDK_KEY_Home ? l->start_index : l->start_index + l->length, shift);
        }
      return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      text_insert (self, "\n");
      return TRUE;
    default:
      {
        gunichar ch = gdk_keyval_to_unicode (keyval);
        if (ch != 0 && g_unichar_isprint (ch))
          {
            char buf[8] = { 0 };
            g_unichar_to_utf8 (ch, buf);
            text_insert (self, buf);
            return TRUE;
          }
      }
    }
  return FALSE;
}

static void
draw_text (EaselCanvas *self, GtkSnapshot *snapshot, double s)
{
  if (!self->text_active && !self->text_creating)
    return;

  EaselRect r = self->text_rect;

  if (self->text_texture != NULL)
    {
      graphene_rect_t at = GRAPHENE_RECT_INIT ((float) (r.x * s), (float) (r.y * s),
                                               (float) (r.width * s), (float) (r.height * s));
      gtk_snapshot_append_scaled_texture (snapshot, self->text_texture, GSK_SCALING_FILTER_NEAREST, &at);
    }

  graphene_rect_t box = GRAPHENE_RECT_INIT ((float) (r.x * s) - 0.5f, (float) (r.y * s) - 0.5f,
                                            (float) (r.width * s) + 1, (float) (r.height * s) + 1);
  GskPathBuilder *builder = gsk_path_builder_new ();
  gsk_path_builder_add_rect (builder, &box);
  GskPath *path = gsk_path_builder_free_to_path (builder);
  const GdkRGBA white = { 1, 1, 1, 1 }, black = { 0, 0, 0, 1 };
  GskStroke *solid = gsk_stroke_new (1.0f), *dashed = gsk_stroke_new (1.0f);
  const float dashes[] = { 4, 4 };

  gsk_stroke_set_dash (dashed, dashes, 2);
  gtk_snapshot_append_stroke (snapshot, path, solid, &white);
  gtk_snapshot_append_stroke (snapshot, path, dashed, &black);
  gsk_stroke_free (solid);
  gsk_stroke_free (dashed);
  gsk_path_unref (path);

  int sa, sb;
  if (text_sel_range (self, &sa, &sb) && self->text_layout != NULL)
    {
      const GdkRGBA tint = { 0.2f, 0.4f, 0.9f, 0.35f };
      const char *str = self->text_buf->str;

      /* One rectangle per character: robust across wrapped and empty lines. */
      for (int i = sa; i < sb; i = (int) (g_utf8_next_char (str + i) - str))
        {
          PangoRectangle pos;
          pango_layout_index_to_pos (self->text_layout, i, &pos);
          int w = pos.width;
          if (str[i] == '\n' || w <= 0)
            w = 4 * PANGO_SCALE; /* a line break shows as a small block */
          graphene_rect_t hl = GRAPHENE_RECT_INIT ((float) ((r.x + pos.x / (double) PANGO_SCALE) * s),
                                                   (float) ((r.y + pos.y / (double) PANGO_SCALE) * s),
                                                   (float) (w / (double) PANGO_SCALE * s),
                                                   (float) (pos.height / (double) PANGO_SCALE * s));
          gtk_snapshot_append_color (snapshot, &tint, &hl);
        }
    }

  if (self->text_active)
    for (int h = 1; h <= 8; h++)
      {
        double cx, cy;
        sel_handle_pos (&r, h, s, &cx, &cy);
        graphene_rect_t halo = GRAPHENE_RECT_INIT ((float) cx - 3.5f, (float) cy - 3.5f, 7, 7);
        graphene_rect_t dot = GRAPHENE_RECT_INIT ((float) cx - 2.5f, (float) cy - 2.5f, 5, 5);
        gtk_snapshot_append_color (snapshot, &white, &halo);
        gtk_snapshot_append_color (snapshot, &black, &dot);
      }

  if (self->text_active && self->text_caret_on && self->text_layout != NULL)
    {
      PangoRectangle pos;
      pango_layout_get_cursor_pos (self->text_layout, self->text_cursor, &pos, NULL);
      graphene_rect_t caret = GRAPHENE_RECT_INIT ((float) ((r.x + pos.x / PANGO_SCALE) * s),
                                                  (float) ((r.y + pos.y / PANGO_SCALE) * s),
                                                  (float) MAX (1.0, s * 0.5),
                                                  (float) (MAX (1, pos.height / PANGO_SCALE) * s));
      gtk_snapshot_append_color (snapshot, &black, &caret);
    }
}

/* Where a pointer at widget (wx, wy) is relative to the text box:
 * 0 outside, 1..8 a resize handle, 9 the border (move), 10 inside (caret). */
static int
text_hit (EaselCanvas *self, double wx, double wy)
{
  if (!self->text_active)
    return 0;
  double s = logical_per_pixel (self);

  for (int h = 1; h <= 8; h++)
    {
      double cx, cy;
      sel_handle_pos (&self->text_rect, h, s, &cx, &cy);
      if (fabs (wx - cx) <= 5 && fabs (wy - cy) <= 5)
        return h;
    }
  double x0 = self->text_rect.x * s, y0 = self->text_rect.y * s;
  double x1 = x0 + self->text_rect.width * s, y1 = y0 + self->text_rect.height * s;

  if (wx < x0 - 4 || wx > x1 + 4 || wy < y0 - 4 || wy > y1 + 4)
    return 0;
  if (wx < x0 + 3 || wx > x1 - 3 || wy < y0 + 3 || wy > y1 - 3)
    return 9;
  return 10;
}

static void
text_resize_to (EaselCanvas *self, int x, int y)
{
  EaselRect o = self->text_orig;
  int l = o.x, t = o.y, r = o.x + o.width, b = o.y + o.height, h = self->text_handle;

  if (h == 1 || h == 4 || h == 6) l = MIN (x, r - 3);
  if (h == 3 || h == 5 || h == 8) r = MAX (x + 1, l + 3);
  if (h <= 3) t = MIN (y, b - 3);
  if (h >= 6) b = MAX (y + 1, t + 3);
  self->text_rect = (EaselRect) { l, t, r - l, b - t };
  text_render (self);
}

static void
text_begin_drag (EaselCanvas *self, int x, int y)
{
  text_commit (self);
  if (self->text_buf == NULL)
    self->text_buf = g_string_new (NULL);
  if (self->text_attr == NULL)
    self->text_attr = g_array_new (FALSE, FALSE, sizeof (TextAttr));
  self->text_creating = TRUE;
  self->text_rect = (EaselRect) { CLAMP (x, 0, self->doc->width - 1), CLAMP (y, 0, self->doc->height - 1), 1, 1 };
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
text_drag_to (EaselCanvas *self, int x0, int y0, int x1, int y1)
{
  int l = CLAMP (MIN (x0, x1), 0, self->doc->width - 1), t = CLAMP (MIN (y0, y1), 0, self->doc->height - 1);
  int r = CLAMP (MAX (x0, x1), 0, self->doc->width - 1), b = CLAMP (MAX (y0, y1), 0, self->doc->height - 1);

  self->text_rect = (EaselRect) { l, t, r - l + 1, b - t + 1 };
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
text_end_drag (EaselCanvas *self)
{
  self->text_creating = FALSE;
  if (self->text_rect.width < 8 || self->text_rect.height < 8)
    {
      /* A click, not a drag: open a box of a comfortable default size. */
      self->text_rect.width = MIN (240, self->doc->width - self->text_rect.x);
      self->text_rect.height = MIN (48, self->doc->height - self->text_rect.y);
      if (self->text_rect.width < 3 || self->text_rect.height < 3)
        {
          text_cancel (self);
          return;
        }
    }
  self->text_active = TRUE;
  self->text_cursor = 0;
  g_string_truncate (self->text_buf, 0);
  g_array_set_size (self->text_attr, 0);
  self->text_anchor = -1;
  text_render (self);
  text_emit_state (self);
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
         || tool == EASEL_TOOL_ELLIPSE || tool == EASEL_TOOL_ROUNDED_RECTANGLE
         || tool == EASEL_TOOL_GRADIENT;
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
static void grad_render (EaselCanvas *self);

static void
update_shape (EaselCanvas *self, int x, int y, gboolean constrain)
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
    case EASEL_TOOL_GRADIENT:
      self->grad_x0 = sx;
      self->grad_y0 = sy;
      self->grad_x1 = x;
      self->grad_y1 = y;
      self->grad_button = self->button;
      grad_render (self);
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

  mark_dirty (self, easel_rect_union (self->shape_prev, now));
  self->shape_prev = now;
}

/* Draws the gradient between its two end points over the saved picture. */
static void
grad_render (EaselCanvas *self)
{
  gboolean primary = self->grad_button == GDK_BUTTON_PRIMARY;
  uint32_t c0 = primary ? self->foreground : self->background;
  uint32_t c1 = primary ? self->background : self->foreground;

  restore_shape_area (self);
  EaselRect now = easel_raster_gradient (self->doc, self->grad_region, self->grad_masked ? self->sel_mask : NULL,
                                         self->grad_x0, self->grad_y0, self->grad_x1, self->grad_y1,
                                         (EaselGradientStyle) self->gradient_style, c0, c1);
  mark_dirty (self, easel_rect_union (self->shape_prev, now));
  self->shape_prev = now;
  gtk_widget_queue_draw (GTK_WIDGET (self));
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

static void
grad_commit (EaselCanvas *self)
{
  if (!self->grad_pending)
    return;
  self->grad_pending = FALSE;
  end_shape_drag (self, TRUE);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* The gradient's line and its two end points, drawn over the picture. */
static void
draw_gradient_guide (EaselCanvas *self, GtkSnapshot *snapshot, double s)
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
      if (text_hit (self, x, y) >= 1 && text_hit (self, x, y) <= 9)
        {
          int hit = text_hit (self, x, y);

          self->text_orig = self->text_rect;
          if (hit == 9)
            {
              self->text_drag = 1;
              self->text_grab_x = self->last_x - self->text_rect.x;
              self->text_grab_y = self->last_y - self->text_rect.y;
            }
          else
            {
              self->text_drag = 2;
              self->text_handle = hit;
            }
        }
      else if (text_hit (self, x, y) == 10)
        {
          /* Click inside the box places the caret; dragging selects. */
          text_move_cursor (self, text_index_at (self, self->last_x, self->last_y), FALSE);
          self->text_drag = 3;
        }
      else
        text_begin_drag (self, self->last_x, self->last_y);
      break;

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
      easel_canvas_set_zoom (self, self->zoom == 100 ? self->zoom_level * 100 : 100,
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

    case EASEL_TOOL_GRADIENT:
      gtk_widget_grab_focus (GTK_WIDGET (self));
      if (self->grad_pending)
        {
          double s = logical_per_pixel (self);
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
          grad_commit (self); /* a press elsewhere accepts it and starts a new one */
        }
      /* Fills the selection if there is one, otherwise the whole picture. */
      if (self->sel_active && self->sel_clip != NULL)
        sel_commit (self);
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
      grad_render (self);
      return;
    }
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
  else if (self->tool == EASEL_TOOL_TEXT)
    {
      if (self->text_drag == 1)
        {
          self->text_rect.x = x - self->text_grab_x;
          self->text_rect.y = y - self->text_grab_y;
          gtk_widget_queue_draw (GTK_WIDGET (self));
        }
      else if (self->text_drag == 2)
        text_resize_to (self, x, y);
      else if (self->text_drag == 3)
        text_move_cursor (self, text_index_at (self, x, y), TRUE);
      else if (self->text_creating)
        text_drag_to (self, self->start_ix, self->start_iy, x, y);
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

  if (self->panning)
    {
      self->panning = FALSE;
      gtk_widget_set_cursor (GTK_WIDGET (self), easel_cursor_for_tool (self->tool));
      return;
    }

  if (self->tool == EASEL_TOOL_TEXT)
    {
      if (self->text_creating)
        text_end_drag (self);
      self->text_drag = 0;
      return;
    }

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
      if (self->grad_drag != 0)
        {
          self->grad_drag = 0; /* still adjustable */
          return;
        }
      if (x == self->start_ix && y == self->start_iy)
        end_shape_drag (self, FALSE);
      else if (self->tool == EASEL_TOOL_GRADIENT)
        {
          update_shape (self, x, y, shift_held (gesture));
          self->grad_pending = TRUE; /* kept until accepted: Enter, another press, a tool change */
        }
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
  if (self->text_active && text_key (self, keyval, state))
    return TRUE;

  if (keyval == GDK_KEY_Delete && self->sel_active)
    {
      easel_canvas_clear_selection (self);
      return TRUE;
    }

  if (self->grad_pending && (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter))
    {
      grad_commit (self);
      return TRUE;
    }
  if (self->grad_pending && keyval == GDK_KEY_Escape)
    {
      end_shape_drag (self, FALSE);
      self->grad_pending = FALSE;
      gtk_widget_queue_draw (GTK_WIDGET (self));
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

  if (self->tool == EASEL_TOOL_TEXT && self->resize_handle == 0 && self->handle_hover == 0)
    {
      static const char *const grips[] = { NULL, "nwse-resize", "ns-resize", "nesw-resize", "ew-resize",
                                           "ew-resize", "nesw-resize", "ns-resize", "nwse-resize" };
      int hit = self->text_drag == 1 ? 9 : self->text_drag == 2 ? self->text_handle
                : self->text_drag == 3 ? 10 : text_hit (self, x, y);

      if (hit >= 1 && hit <= 8)
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self), grips[hit]);
      else if (hit == 9)
        gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "move");
      else if (hit == 10)
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
  text_free_render (self);
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

  signals[SIGNAL_POINTER_MOVED] =
    g_signal_new ("pointer-moved", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
  signals[SIGNAL_HISTORY_CHANGED] =
    g_signal_new ("history-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_SELECTION_CHANGED] =
    g_signal_new ("selection-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_TEXT_STATE] =
    g_signal_new ("text-state", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_UINT);
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

/* One notch in (+1) or out (-1); (ix, iy) is the picture pixel to keep in
 * view, or negative for the middle of the window. */
void
easel_canvas_zoom_step (EaselCanvas *self, int direction, int ix, int iy)
{
  static const int steps[] = { 10, 15, 20, 25, 33, 50, 67, 100, 200, 300, 400, 600, 800 };
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
  self->font = g_strdup ("Arial 11");
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
    grad_render (self);
  text_apply_color (self, argb);
}

void
easel_canvas_set_background (EaselCanvas *self, uint32_t argb)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->background = argb;
  if (self->grad_pending)
    grad_render (self);
  text_render (self);
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
  zoom = CLAMP (zoom, 10, 800);
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

/* ---- Image menu: flip, rotate, stretch, skew, invert ------------------- */

typedef struct
{
  int      stretch_h, stretch_v;  /* percent, 100 = unchanged */
  double   skew_h, skew_v;        /* degrees */
  gboolean flip_h, flip_v, invert;
  int      turns;                 /* clockwise quarter turns */
} Transform;

typedef struct { void *data; int w, h; size_t elem; } Buf;

static void
buf_replace (Buf *b, void *data, int w, int h)
{
  g_free (b->data);
  b->data = data;
  b->w = w;
  b->h = h;
}

/* Runs the transform over the pixels (and the matching mask, if any). */
static gboolean
run_transform (const Transform *t, Buf *pix, Buf *mask, uint32_t pix_fill)
{
  uint8_t mask_fill = 0;
  void *out;
  int nw, nh;

  if (t->invert)
    easel_invert (pix->data, (size_t) pix->w * pix->h);

  if (t->stretch_h != 100 || t->stretch_v != 100)
    {
      nw = MAX (1, (int) ((gint64) pix->w * t->stretch_h / 100));
      nh = MAX (1, (int) ((gint64) pix->h * t->stretch_v / 100));
      if (nw > 16384 || nh > 16384)
        return FALSE;
      out = easel_scale (pix->data, pix->w, pix->h, 4, nw, nh);
      if (out == NULL)
        return FALSE;
      if (mask != NULL)
        {
          void *mout = easel_scale (mask->data, mask->w, mask->h, 1, nw, nh);
          if (mout == NULL)
            {
              free (out);
              return FALSE;
            }
          buf_replace (mask, mout, nw, nh);
        }
      buf_replace (pix, out, nw, nh);
    }

  for (int axis = 0; axis < 2; axis++)
    {
      double deg = axis == 0 ? t->skew_h : t->skew_v;
      if (deg == 0)
        continue;
      out = easel_skew (pix->data, pix->w, pix->h, 4, deg, axis == 0, &pix_fill, &nw, &nh);
      if (out == NULL || nw > 16384 || nh > 16384)
        {
          free (out);
          return FALSE;
        }
      if (mask != NULL)
        {
          void *mout = easel_skew (mask->data, mask->w, mask->h, 1, deg, axis == 0, &mask_fill, &nw, &nh);
          if (mout == NULL)
            {
              free (out);
              return FALSE;
            }
          buf_replace (mask, mout, nw, nh);
        }
      buf_replace (pix, out, nw, nh);
    }

  if (t->flip_h)
    {
      easel_flip (pix->data, pix->w, pix->h, 4, 1);
      if (mask != NULL)
        easel_flip (mask->data, mask->w, mask->h, 1, 1);
    }
  if (t->flip_v)
    {
      easel_flip (pix->data, pix->w, pix->h, 4, 0);
      if (mask != NULL)
        easel_flip (mask->data, mask->w, mask->h, 1, 0);
    }
  if (t->turns % 4 != 0)
    {
      out = easel_rotate (pix->data, pix->w, pix->h, 4, t->turns, &nw, &nh);
      if (out == NULL)
        return FALSE;
      if (mask != NULL)
        {
          int mw, mh;
          void *mout = easel_rotate (mask->data, mask->w, mask->h, 1, t->turns, &mw, &mh);
          if (mout == NULL)
            {
              free (out);
              return FALSE;
            }
          buf_replace (mask, mout, mw, mh);
        }
      buf_replace (pix, out, nw, nh);
    }
  return TRUE;
}

/* Applies to the selection if there is one, otherwise to the whole picture. */
static void
apply_transform (EaselCanvas *self, const Transform *t)
{
  if (self->text_active || self->text_creating)
    text_commit (self);
  commit_seq (self, FALSE);

  if (self->sel_active)
    {
      sel_lift (self, FALSE);
      if (self->sel_clip == NULL)
        return;

      Buf pix = { g_memdup2 (self->sel_clip->pixels, (gsize) self->sel_clip->width * self->sel_clip->height * 4),
                  self->sel_clip->width, self->sel_clip->height, 4 };
      Buf mask = { NULL, pix.w, pix.h, 1 };

      if (self->sel_clip->mask != NULL)
        mask.data = g_memdup2 (self->sel_clip->mask, (gsize) pix.w * pix.h);
      else if (t->skew_h != 0 || t->skew_v != 0)
        {
          mask.data = g_malloc (pix.w * (gsize) pix.h); /* skewing leaves empty corners to hide */
          memset (mask.data, 1, pix.w * (gsize) pix.h);
        }

      if (run_transform (t, &pix, mask.data != NULL ? &mask : NULL, 0))
        {
          EaselClip *clip = easel_clip_new (pix.w, pix.h);
          if (clip != NULL)
            {
              memcpy (clip->pixels, pix.data, (gsize) pix.w * pix.h * 4);
              if (mask.data != NULL)
                clip->mask = g_steal_pointer (&mask.data);
              easel_clip_free (self->sel_clip);
              self->sel_clip = clip;
              self->sel_rect.width = pix.w;
              self->sel_rect.height = pix.h;
              g_clear_pointer (&self->sel_path, g_array_unref);
              g_clear_pointer (&self->sel_mask, g_free);
              sel_refresh_texture (self);
            }
        }
      g_free (pix.data);
      g_free (mask.data);
      return;
    }

  int ow = self->doc->width, oh = self->doc->height;
  g_autofree uint32_t *old = g_memdup2 (self->doc->pixels, (gsize) ow * oh * 4);
  Buf pix = { g_memdup2 (old, (gsize) ow * oh * 4), ow, oh, 4 };

  if (run_transform (t, &pix, NULL, self->background)
      && easel_document_replace (self->doc, pix.w, pix.h, pix.data))
    {
      easel_history_push_full (self->history, old, ow, oh, self->doc);
      gtk_widget_queue_resize (GTK_WIDGET (self));
      mark_all_dirty (self);
      g_signal_emit (self, signals[SIGNAL_HISTORY_CHANGED], 0);
    }
  g_free (pix.data);
}

void
easel_canvas_turn (EaselCanvas *self, EaselTurn turn)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  Transform t = { 100, 100, 0, 0, FALSE, FALSE, FALSE, 0 };

  switch (turn)
    {
    case EASEL_FLIP_H: t.flip_h = TRUE; break;
    case EASEL_FLIP_V: t.flip_v = TRUE; break;
    case EASEL_ROTATE_90: t.turns = 1; break;
    case EASEL_ROTATE_180: t.turns = 2; break;
    case EASEL_ROTATE_270: t.turns = 3; break;
    }
  apply_transform (self, &t);
}

void
easel_canvas_invert (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  Transform t = { 100, 100, 0, 0, FALSE, FALSE, TRUE, 0 };
  apply_transform (self, &t);
}

void
easel_canvas_black_and_white (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (self->text_active || self->text_creating)
    text_commit (self);
  commit_seq (self, FALSE);
  easel_canvas_flush (self);

  int w = self->doc->width, h = self->doc->height;
  g_autofree uint32_t *old = g_memdup2 (self->doc->pixels, (gsize) w * h * 4);

  easel_black_and_white (self->doc->pixels, (size_t) w * h);
  if (easel_history_push (self->history, self->doc, old, (EaselRect) { 0, 0, w, h }))
    {
      mark_all_dirty (self);
      g_signal_emit (self, signals[SIGNAL_HISTORY_CHANGED], 0);
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
easel_canvas_stretch_skew (EaselCanvas *self, int stretch_h, int stretch_v, double skew_h, double skew_v)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  Transform t = { stretch_h, stretch_v, skew_h, skew_v, FALSE, FALSE, FALSE, 0 };

  if (stretch_h == 100 && stretch_v == 100 && skew_h == 0 && skew_v == 0)
    return;
  apply_transform (self, &t);
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
  int a, b;
  return self->sel_active || text_sel_range (self, &a, &b);
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
easel_canvas_set_text_opaque (EaselCanvas *self, gboolean opaque)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (self->text_opaque == opaque)
    return;
  self->text_opaque = opaque;
  text_render (self);
}

void
easel_canvas_set_gradient_style (EaselCanvas *self, int style)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->gradient_style = style;
}

void
easel_canvas_set_text_smooth (EaselCanvas *self, gboolean smooth)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (self->text_smooth == smooth)
    return;
  self->text_smooth = smooth;
  text_render (self);
}

void
easel_canvas_text_apply_style (EaselCanvas *self, guint flag, gboolean on)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  text_apply_style (self, flag, on);
}

gboolean
easel_canvas_text_active (EaselCanvas *self)
{
  g_return_val_if_fail (EASEL_IS_CANVAS (self), FALSE);
  return self->text_active;
}

void
easel_canvas_text_select_all (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (!self->text_active)
    return;
  self->text_anchor = 0;
  self->text_cursor = (int) self->text_buf->len;
  text_emit_state (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
easel_canvas_text_copy (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  int a, b;

  if (!text_sel_range (self, &a, &b))
    return;
  g_autofree char *piece = g_strndup (self->text_buf->str + a, (gsize) (b - a));
  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (self)), piece);
}

void
easel_canvas_text_delete (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (text_delete_selection (self))
    {
      text_render (self);
      text_emit_state (self);
    }
}

void
easel_canvas_text_cut (EaselCanvas *self)
{
  easel_canvas_text_copy (self);
  easel_canvas_text_delete (self);
}

static void
on_clipboard_text (GObject *source, GAsyncResult *result, gpointer data)
{
  EaselCanvas *self = data;
  g_autoptr (GError) error = NULL;
  g_autofree char *text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (source), result, &error);

  if (text != NULL && self->text_active)
    text_insert (self, text);
  g_object_unref (self);
}

void
easel_canvas_text_paste (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  gdk_clipboard_read_text_async (gtk_widget_get_clipboard (GTK_WIDGET (self)), NULL,
                                 on_clipboard_text, g_object_ref (self));
}

void
easel_canvas_set_font (EaselCanvas *self, const char *description)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  if (g_strcmp0 (self->font, description) == 0)
    return;
  g_free (self->font);
  self->font = g_strdup (description);
  text_render (self);
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
