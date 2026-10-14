/* The canvas widget: presents an EaselDocument and turns pointer input into
 * calls on the core engine. It never owns drawing logic itself. */
#pragma once

#include <gtk/gtk.h>

#include "core/document.h"
#include "core/raster.h"
#include "ui/tool.h"

G_BEGIN_DECLS

#define EASEL_TYPE_CANVAS (easel_canvas_get_type ())
G_DECLARE_FINAL_TYPE (EaselCanvas, easel_canvas, EASEL, CANVAS, GtkWidget)

/* Takes ownership of `doc`. */
GtkWidget     *easel_canvas_new (EaselDocument *doc);
EaselDocument *easel_canvas_get_document (EaselCanvas *self);

void easel_canvas_set_foreground (EaselCanvas *self, uint32_t argb);
void easel_canvas_set_background (EaselCanvas *self, uint32_t argb);
void easel_canvas_set_tool (EaselCanvas *self, EaselTool tool);
void easel_canvas_set_tool_size (EaselCanvas *self, int size);
void easel_canvas_set_brush_shape (EaselCanvas *self, EaselBrushShape shape);
void easel_canvas_set_line_width (EaselCanvas *self, int width);
void easel_canvas_set_fill_style (EaselCanvas *self, EaselFillStyle style);

void easel_canvas_set_zoom (EaselCanvas *self, int zoom, int ix, int iy);
int  easel_canvas_get_zoom (EaselCanvas *self);
void easel_canvas_set_zoom_level (EaselCanvas *self, int level); /* what a Magnifier click applies */

/* Call after changing the document from outside the canvas. */
void easel_canvas_invalidate (EaselCanvas *self);

/* Signals:
 *  "pointer-moved" (int x, int y): image coordinates, or -1,-1 when the
 *                  pointer leaves the canvas.
 *  "color-picked"  (guint argb, gboolean foreground): Pick Color released. */

G_END_DECLS
