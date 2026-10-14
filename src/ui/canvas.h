/* The canvas widget: presents an EaselDocument and turns pointer input into
 * calls on the core engine. It never owns drawing logic itself. */
#pragma once

#include <gtk/gtk.h>

#include "core/clip.h"
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
void easel_canvas_zoom_step (EaselCanvas *self, int direction, int ix, int iy);
guint64 easel_canvas_get_change_count (EaselCanvas *self); /* see window thumbnail */
gint64 easel_canvas_get_paint_time (EaselCanvas *self); /* microseconds */
void easel_canvas_set_show_grid (EaselCanvas *self, gboolean show); /* drawn at 4x and above */
void easel_canvas_set_zoom_level (EaselCanvas *self, int level); /* what a Magnifier click applies */

void     easel_canvas_mark_saved (EaselCanvas *self);
gboolean easel_canvas_is_saved (EaselCanvas *self);
gboolean easel_canvas_can_undo (EaselCanvas *self);
gboolean easel_canvas_can_redo (EaselCanvas *self);
gboolean easel_canvas_undo (EaselCanvas *self);
gboolean easel_canvas_redo (EaselCanvas *self);
void     easel_canvas_clear (EaselCanvas *self, uint32_t argb); /* undoable */

/* Selection (Select tool) and clipboard. */
gboolean easel_canvas_has_selection (EaselCanvas *self);
void     easel_canvas_flush (EaselCanvas *self); /* put a floating selection down */
void     easel_canvas_select_all (EaselCanvas *self);
EaselClip *easel_canvas_get_selection (EaselCanvas *self); /* a copy; caller frees */
void     easel_canvas_set_font (EaselCanvas *self, const char *description);
void     easel_canvas_set_text_opaque (EaselCanvas *self, gboolean opaque);
void     easel_canvas_set_text_smooth (EaselCanvas *self, gboolean smooth);
void     easel_canvas_set_gradient_style (EaselCanvas *self, int style); /* EaselGradientStyle */
void     easel_canvas_text_apply_style (EaselCanvas *self, guint flag, gboolean on);
gboolean easel_canvas_text_active (EaselCanvas *self);
void     easel_canvas_text_select_all (EaselCanvas *self);
void     easel_canvas_text_copy (EaselCanvas *self);
void     easel_canvas_text_cut (EaselCanvas *self);
void     easel_canvas_text_delete (EaselCanvas *self);
void     easel_canvas_text_paste (EaselCanvas *self);
void     easel_canvas_copy (EaselCanvas *self);
void     easel_canvas_cut (EaselCanvas *self);
void     easel_canvas_clear_selection (EaselCanvas *self);
void     easel_canvas_paste (EaselCanvas *self);
void     easel_canvas_paste_clip (EaselCanvas *self, EaselClip *clip); /* takes ownership */
void     easel_canvas_set_opaque (EaselCanvas *self, gboolean opaque); /* Draw Opaque */

/* Crops or extends the picture (new area in the background colour); undoable. */
gboolean easel_canvas_resize (EaselCanvas *self, int width, int height);

/* Image menu. They act on the selection when there is one, else on the picture. */
typedef enum { EASEL_FLIP_H, EASEL_FLIP_V, EASEL_ROTATE_90, EASEL_ROTATE_180, EASEL_ROTATE_270 } EaselTurn;
void     easel_canvas_turn (EaselCanvas *self, EaselTurn turn);
void     easel_canvas_invert (EaselCanvas *self);
void     easel_canvas_black_and_white (EaselCanvas *self);
void     easel_canvas_stretch_skew (EaselCanvas *self, int stretch_h, int stretch_v, double skew_h, double skew_v);

/* Replaces the picture; takes ownership, clears undo history. */
void easel_canvas_set_document (EaselCanvas *self, EaselDocument *doc);

/* Call after changing the document from outside the canvas. */
void easel_canvas_invalidate (EaselCanvas *self);

/* Signals:
 *  "pointer-moved" (int x, int y): image coordinates, or -1,-1 when the
 *                  pointer leaves the canvas.
 *  "history-changed": undo/redo availability may have changed.
 *  "selection-changed": a selection appeared, moved into place or went away.
 *  "zoom-changed": the magnification changed.
 *  "resize-preview" (int w, int h): size while a resize handle is dragged;
 *                  -1,-1 when the drag ends.
 *  "color-picked"  (guint argb, gboolean foreground): Pick Color released. */

G_END_DECLS
