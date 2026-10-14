#include "ui/tool.h"

#include <glib/gi18n.h>

/* Everything the UI knows about each tool, in one place. Order follows the
 * enum (the classic two-column toolbox, row by row). */
static const EaselToolInfo tools[EASEL_N_TOOLS] = {
  [EASEL_TOOL_GRADIENT] = { N_("Gradient"), "easel-gradient-symbolic", "Gr",
    N_("Fills with a blend from the foreground to the background color. Drag to set direction and length."),
    EASEL_TOOL_SHAPE },
  [EASEL_TOOL_PAN] = { N_("Pan"), "easel-pan-symbolic", "Pa",
    N_("Click and drag to navigate the image."),
    0 },
  [EASEL_TOOL_FREE_FORM_SELECT] = { N_("Free-Form Select"), "easel-free-form-select-symbolic", "FS",
    N_("Selects a free-form part of the picture to move, copy, or edit."),
    EASEL_TOOL_SELECTION },
  [EASEL_TOOL_SELECT] = { N_("Select"), "easel-select-symbolic", "Se",
    N_("Selects a rectangular part of the picture to move, copy, or edit."),
    EASEL_TOOL_SELECTION },
  [EASEL_TOOL_ERASER] = { N_("Eraser"), "easel-eraser-symbolic", "Er",
    N_("Erases a portion of the picture. Right button: replaces the foreground color with the background color."),
    EASEL_TOOL_HOVER_PREVIEW },
  [EASEL_TOOL_FILL] = { N_("Fill With Color"), "easel-fill-symbolic", "Fl",
    N_("Fills an area with the foreground color (left button) or the background color (right button)."),
    0 },
  [EASEL_TOOL_PICK_COLOR] = { N_("Pick Color"), "easel-pick-color-symbolic", "Pk",
    N_("Picks up a color from the picture for drawing. Left button: foreground. Right button: background."),
    0 },
  [EASEL_TOOL_MAGNIFIER] = { N_("Magnifier"), "easel-magnifier-symbolic", "Mg",
    N_("Changes the magnification."),
    0 },
  [EASEL_TOOL_PENCIL] = { N_("Pencil"), "easel-pencil-symbolic", "Pe",
    N_("Draws a free-form line. Left button: foreground color. Right button: background color."),
    EASEL_TOOL_HOVER_PREVIEW },
  [EASEL_TOOL_BRUSH] = { N_("Brush"), "easel-brush-symbolic", "Br",
    N_("Draws using a brush with the selected shape and size."),
    EASEL_TOOL_HOVER_PREVIEW },
  [EASEL_TOOL_AIRBRUSH] = { N_("Airbrush"), "easel-airbrush-symbolic", "Ai",
    N_("Draws using an airbrush of the selected size."),
    0 },
  [EASEL_TOOL_TEXT] = { N_("Text"), "easel-text-symbolic", "Tx",
    N_("Inserts text into the picture."),
    0 },
  [EASEL_TOOL_LINE] = { N_("Line"), "easel-line-symbolic", "Ln",
    N_("Draws a straight line with the selected line width."),
    EASEL_TOOL_SHAPE },
  [EASEL_TOOL_CURVE] = { N_("Curve"), "easel-curve-symbolic", "Cv",
    N_("Draws a curved line with the selected line width."),
    0 },
  [EASEL_TOOL_RECTANGLE] = { N_("Rectangle"), "easel-rectangle-symbolic", "Re",
    N_("Draws a rectangle with the selected fill style."),
    EASEL_TOOL_SHAPE },
  [EASEL_TOOL_POLYGON] = { N_("Polygon"), "easel-polygon-symbolic", "Po",
    N_("Draws a polygon with the selected fill style."),
    0 },
  [EASEL_TOOL_ELLIPSE] = { N_("Ellipse"), "easel-ellipse-symbolic", "El",
    N_("Draws an ellipse with the selected fill style."),
    EASEL_TOOL_SHAPE },
  [EASEL_TOOL_ROUNDED_RECTANGLE] = { N_("Rounded Rectangle"), "easel-rounded-rectangle-symbolic", "RR",
    N_("Draws a rounded rectangle with the selected fill style."),
    EASEL_TOOL_SHAPE },
};

const EaselToolInfo *
easel_tool_info (EaselTool tool)
{
  return &tools[tool];
}
