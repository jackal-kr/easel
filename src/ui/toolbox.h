/* Left tool strip: the 16 classic tools in two columns, with an options area
 * below that follows the selected tool. Tools that are not implemented yet
 * are shown disabled. */
#pragma once

#include <gtk/gtk.h>

#include "core/raster.h"
#include "ui/tool.h"

G_BEGIN_DECLS

#define EASEL_TYPE_TOOLBOX (easel_toolbox_get_type ())
G_DECLARE_FINAL_TYPE (EaselToolbox, easel_toolbox, EASEL, TOOLBOX, GtkWidget)

GtkWidget  *easel_toolbox_new (void);
EaselTool   easel_toolbox_get_tool (EaselToolbox *self);
const char *easel_toolbox_get_tool_hint (EaselToolbox *self);

/* Size of the current tool (pencil, brush or eraser); 1 for other tools. */
int             easel_toolbox_get_tool_size (EaselToolbox *self);
EaselBrushShape easel_toolbox_get_brush_shape (EaselToolbox *self);

/* Shared by Line and the shape tools (as in classic Paint). */
int            easel_toolbox_get_line_width (EaselToolbox *self);
EaselFillStyle easel_toolbox_get_fill_style (EaselToolbox *self);
const char    *easel_toolbox_get_font (EaselToolbox *self); /* Pango description */
void           easel_toolbox_set_text_style (EaselToolbox *self, guint flags); /* EASEL_TEXT_* */
gboolean       easel_toolbox_get_text_opaque (EaselToolbox *self);
gboolean       easel_toolbox_get_text_smooth (EaselToolbox *self);
gboolean       easel_toolbox_get_opaque (EaselToolbox *self); /* Draw Opaque (selections) */
int            easel_toolbox_get_zoom_level (EaselToolbox *self);

void easel_toolbox_select_tool (EaselToolbox *self, EaselTool tool);
/* Used by Pick Color, which hands control back to the tool used before it. */
void easel_toolbox_select_previous_tool (EaselToolbox *self);

/* Signals: "tool-changed" and "options-changed" (no arguments). */

G_END_DECLS
