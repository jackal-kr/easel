#pragma once

/* The 16 classic tools plus Pan and Gradient, in toolbox order (two columns,
 * row by row): navigate, select, erase/colour, freehand, text, lines, shapes. */
typedef enum
{
  EASEL_TOOL_PAN,
  EASEL_TOOL_MAGNIFIER,
  EASEL_TOOL_SELECT,
  EASEL_TOOL_FREE_FORM_SELECT,
  EASEL_TOOL_ERASER,
  EASEL_TOOL_PICK_COLOR,
  EASEL_TOOL_FILL,
  EASEL_TOOL_GRADIENT,
  EASEL_TOOL_PENCIL,
  EASEL_TOOL_BRUSH,
  EASEL_TOOL_AIRBRUSH,
  EASEL_TOOL_TEXT,
  EASEL_TOOL_LINE,
  EASEL_TOOL_CURVE,
  EASEL_TOOL_RECTANGLE,
  EASEL_TOOL_ROUNDED_RECTANGLE,
  EASEL_TOOL_ELLIPSE,
  EASEL_TOOL_POLYGON,
  EASEL_N_TOOLS
} EaselTool;

/* Character styles in the Text tool. */
enum { EASEL_TEXT_BOLD = 1, EASEL_TEXT_ITALIC = 2, EASEL_TEXT_UNDERLINE = 4, EASEL_TEXT_STRIKE = 8 };
