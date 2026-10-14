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

/* What kind of tool it is, for code that treats groups of tools alike. */
enum
{
  EASEL_TOOL_SELECTION     = 1 << 0, /* Select, Free-Form Select */
  EASEL_TOOL_SHAPE         = 1 << 1, /* dragged out with a live preview: Line, boxes, Gradient */
  EASEL_TOOL_HOVER_PREVIEW = 1 << 2, /* shows its footprint under the pointer */
};

typedef struct
{
  const char *name;
  const char *icon;  /* symbolic icon name; the toolbox falls back to `abbr` if the theme lacks it */
  const char *abbr;
  const char *hint;  /* status bar text */
  unsigned    flags; /* EASEL_TOOL_SELECTION, ... */
} EaselToolInfo;

const EaselToolInfo *easel_tool_info (EaselTool tool);

/* Character styles in the Text tool. */
enum { EASEL_TEXT_BOLD = 1, EASEL_TEXT_ITALIC = 2, EASEL_TEXT_UNDERLINE = 4, EASEL_TEXT_STRIKE = 8 };
