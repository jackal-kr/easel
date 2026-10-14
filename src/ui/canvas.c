#include "ui/canvas.h"

#include <math.h>

#include "core/raster.h"

struct _EaselCanvas
{
  GtkWidget parent_instance;

  EaselDocument *doc;
  GdkTexture    *texture; /* last uploaded snapshot of doc */
  cairo_region_t *dirty;  /* image pixels changed since `texture` was built */

  int      zoom; /* integer magnification, 1 = actual size */
  uint32_t foreground;
  uint32_t background;
  int      brush_size;

  /* Stroke in progress. */
  gboolean drawing;
  uint32_t stroke_color;
  double   start_x, start_y; /* widget coordinates where the drag began */
  int      last_x, last_y;   /* last image pixel drawn */
};

G_DEFINE_FINAL_TYPE (EaselCanvas, easel_canvas, GTK_TYPE_WIDGET)

enum { SIGNAL_POINTER_MOVED, N_SIGNALS };
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
      cairo_rectangle_int_t cr = { r.x, r.y, r.width, r.height };
      if (self->dirty == NULL)
        self->dirty = cairo_region_create_rectangle (&cr);
      else
        cairo_region_union_rectangle (self->dirty, &cr);
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
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

  /* Left button draws with the foreground colour, right with the background. */
  self->stroke_color = button == GDK_BUTTON_PRIMARY ? self->foreground : self->background;
  self->start_x = x;
  self->start_y = y;
  to_image (self, x, y, &self->last_x, &self->last_y);
  self->drawing = TRUE;

  mark_dirty (self, easel_raster_stamp (self->doc, self->last_x, self->last_y,
                                        self->brush_size, self->stroke_color));
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

  /* Join consecutive samples with a line so fast strokes have no gaps. */
  EaselRect touched = easel_raster_stroke (self->doc, self->last_x, self->last_y, x, y,
                                           self->brush_size, self->stroke_color);
  self->last_x = x;
  self->last_y = y;
  mark_dirty (self, touched);
}

static void
on_drag_end (GtkGestureDrag *gesture, double offset_x, double offset_y, EaselCanvas *self)
{
  self->drawing = FALSE;
}

static void
on_motion (GtkEventControllerMotion *controller, double x, double y, EaselCanvas *self)
{
  int ix, iy;
  to_image (self, x, y, &ix, &iy);
  g_signal_emit (self, signals[SIGNAL_POINTER_MOVED], 0, ix, iy);
}

static void
on_leave (GtkEventControllerMotion *controller, EaselCanvas *self)
{
  g_signal_emit (self, signals[SIGNAL_POINTER_MOVED], 0, -1, -1);
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
}

static void
easel_canvas_init (EaselCanvas *self)
{
  self->zoom = 1;
  self->foreground = EASEL_RGB (0, 0, 0);
  self->background = EASEL_RGB (255, 255, 255);
  self->brush_size = 1;

  gtk_widget_add_css_class (GTK_WIDGET (self), "easel-canvas");
  gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "crosshair");

  GtkGesture *drag = gtk_gesture_drag_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), 0); /* any button */
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));

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
easel_canvas_set_brush_size (EaselCanvas *self, int size)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  self->brush_size = CLAMP (size, 1, EASEL_MAX_BRUSH);
}

void
easel_canvas_invalidate (EaselCanvas *self)
{
  g_return_if_fail (EASEL_IS_CANVAS (self));
  mark_all_dirty (self);
}
