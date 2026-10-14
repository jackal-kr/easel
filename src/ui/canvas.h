/* The canvas widget: presents an EaselDocument and turns pointer input into
 * calls on the core engine. It never owns drawing logic itself. */
#pragma once

#include <gtk/gtk.h>

#include "core/document.h"

G_BEGIN_DECLS

#define EASEL_TYPE_CANVAS (easel_canvas_get_type ())
G_DECLARE_FINAL_TYPE (EaselCanvas, easel_canvas, EASEL, CANVAS, GtkWidget)

/* Takes ownership of `doc`. */
GtkWidget     *easel_canvas_new (EaselDocument *doc);
EaselDocument *easel_canvas_get_document (EaselCanvas *self);

void easel_canvas_set_foreground (EaselCanvas *self, uint32_t argb);
void easel_canvas_set_background (EaselCanvas *self, uint32_t argb);
void easel_canvas_set_brush_size (EaselCanvas *self, int size);

/* Call after changing the document from outside the canvas. */
void easel_canvas_invalidate (EaselCanvas *self);

/* Signal "pointer-moved" (int x, int y): image coordinates, or -1,-1 when the
 * pointer leaves the canvas. */

G_END_DECLS
