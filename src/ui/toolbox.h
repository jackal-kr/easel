/* Left tool strip: the 16 classic tools in two columns, plus the options
 * area below them. Tools that are not implemented yet are shown disabled. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum
{
  EASEL_TOOL_FREE_FORM_SELECT,
  EASEL_TOOL_SELECT,
  EASEL_TOOL_ERASER,
  EASEL_TOOL_FILL,
  EASEL_TOOL_PICK_COLOR,
  EASEL_TOOL_MAGNIFIER,
  EASEL_TOOL_PENCIL,
  EASEL_TOOL_BRUSH,
  EASEL_TOOL_AIRBRUSH,
  EASEL_TOOL_TEXT,
  EASEL_TOOL_LINE,
  EASEL_TOOL_CURVE,
  EASEL_TOOL_RECTANGLE,
  EASEL_TOOL_POLYGON,
  EASEL_TOOL_ELLIPSE,
  EASEL_TOOL_ROUNDED_RECTANGLE,
  EASEL_N_TOOLS
} EaselTool;

#define EASEL_TYPE_TOOLBOX (easel_toolbox_get_type ())
G_DECLARE_FINAL_TYPE (EaselToolbox, easel_toolbox, EASEL, TOOLBOX, GtkBox)

GtkWidget  *easel_toolbox_new (void);
EaselTool   easel_toolbox_get_tool (EaselToolbox *self);
int         easel_toolbox_get_brush_size (EaselToolbox *self);
const char *easel_toolbox_get_tool_hint (EaselToolbox *self);

/* Signals: "tool-changed" and "brush-size-changed" (no arguments). */

G_END_DECLS
