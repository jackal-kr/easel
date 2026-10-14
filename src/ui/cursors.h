/* Per-tool mouse cursors, drawn with Cairo so they need no icon files: a
 * small crosshair marking the exact pixel plus a glyph of the tool. */
#pragma once

#include <gtk/gtk.h>

#include "ui/tool.h"

/* Never NULL; cached, owned by this module. */
GdkCursor *easel_cursor_for_tool (EaselTool tool);
