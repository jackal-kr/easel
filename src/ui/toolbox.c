#include "ui/toolbox.h"

typedef struct
{
  const char *name;
  const char *icon;  /* stock symbolic icon; falls back to `abbr` if the theme lacks it */
  const char *abbr;
  const char *hint;
  gboolean    implemented;
} ToolDef;

/* Order matches the classic two-column layout, row by row. */
static const ToolDef tools[EASEL_N_TOOLS] = {
  [EASEL_TOOL_FREE_FORM_SELECT] = { "Free-Form Select", "edit-select-lasso-symbolic", "FS",
    "Selects a free-form part of the picture to move, copy, or edit.", FALSE },
  [EASEL_TOOL_SELECT] = { "Select", "edit-select-all-symbolic", "Se",
    "Selects a rectangular part of the picture to move, copy, or edit.", FALSE },
  [EASEL_TOOL_ERASER] = { "Eraser", "edit-clear-all-symbolic", "Er",
    "Erases a portion of the picture. Right button: replaces the foreground color with the background color.", FALSE },
  [EASEL_TOOL_FILL] = { "Fill With Color", "color-fill-symbolic", "Fl",
    "Fills an area with the current drawing color.", FALSE },
  [EASEL_TOOL_PICK_COLOR] = { "Pick Color", "color-select-symbolic", "Pk",
    "Picks up a color from the picture for drawing.", FALSE },
  [EASEL_TOOL_MAGNIFIER] = { "Magnifier", "zoom-in-symbolic", "Mg",
    "Changes the magnification.", FALSE },
  [EASEL_TOOL_PENCIL] = { "Pencil", "document-edit-symbolic", "Pe",
    "Draws a free-form line. Left button: foreground color. Right button: background color.", TRUE },
  [EASEL_TOOL_BRUSH] = { "Brush", "applications-graphics-symbolic", "Br",
    "Draws using a brush with the selected shape and size.", FALSE },
  [EASEL_TOOL_AIRBRUSH] = { "Airbrush", "weather-showers-symbolic", "Ai",
    "Draws using an airbrush of the selected size.", FALSE },
  [EASEL_TOOL_TEXT] = { "Text", "insert-text-symbolic", "Tx",
    "Inserts text into the picture.", FALSE },
  [EASEL_TOOL_LINE] = { "Line", "list-remove-symbolic", "Ln",
    "Draws a straight line with the selected line width.", FALSE },
  [EASEL_TOOL_CURVE] = { "Curve", "media-playlist-shuffle-symbolic", "Cv",
    "Draws a curved line with the selected line width.", FALSE },
  [EASEL_TOOL_RECTANGLE] = { "Rectangle", "checkbox-symbolic", "Re",
    "Draws a rectangle with the selected fill style.", FALSE },
  [EASEL_TOOL_POLYGON] = { "Polygon", "emblem-shared-symbolic", "Po",
    "Draws a polygon with the selected fill style.", FALSE },
  [EASEL_TOOL_ELLIPSE] = { "Ellipse", "media-record-symbolic", "El",
    "Draws an ellipse with the selected fill style.", FALSE },
  [EASEL_TOOL_ROUNDED_RECTANGLE] = { "Rounded Rectangle", "checkbox-symbolic", "RR",
    "Draws a rounded rectangle with the selected fill style.", FALSE },
};

static const int sizes[] = { 1, 2, 3, 4, 6, 8, 12, 16 };

struct _EaselToolbox
{
  GtkBox parent_instance;

  GtkWidget *tool_buttons[EASEL_N_TOOLS];
  EaselTool  tool;
  int        brush_size;
};

G_DEFINE_FINAL_TYPE (EaselToolbox, easel_toolbox, GTK_TYPE_BOX)

enum { SIGNAL_TOOL_CHANGED, SIGNAL_BRUSH_SIZE_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
on_tool_toggled (GtkToggleButton *button, EaselToolbox *self)
{
  if (!gtk_toggle_button_get_active (button))
    return;

  self->tool = (EaselTool) GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "easel-tool"));
  g_signal_emit (self, signals[SIGNAL_TOOL_CHANGED], 0);
}

static void
on_size_toggled (GtkToggleButton *button, EaselToolbox *self)
{
  if (!gtk_toggle_button_get_active (button))
    return;

  self->brush_size = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "easel-size"));
  g_signal_emit (self, signals[SIGNAL_BRUSH_SIZE_CHANGED], 0);
}

static GtkWidget *
make_tool_button (EaselTool id, GtkIconTheme *theme)
{
  const ToolDef *def = &tools[id];
  GtkWidget *button = gtk_toggle_button_new ();

  if (gtk_icon_theme_has_icon (theme, def->icon))
    gtk_button_set_icon_name (GTK_BUTTON (button), def->icon);
  else
    {
      g_debug ("no icon '%s' for tool '%s', using text label", def->icon, def->name);
      gtk_button_set_label (GTK_BUTTON (button), def->abbr);
    }

  gtk_widget_add_css_class (button, "flat");
  gtk_widget_set_tooltip_text (button, def->name);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  def->name, -1);
  gtk_widget_set_sensitive (button, def->implemented);
  g_object_set_data (G_OBJECT (button), "easel-tool", GINT_TO_POINTER (id));
  return button;
}

static void
easel_toolbox_class_init (EaselToolboxClass *klass)
{
  signals[SIGNAL_TOOL_CHANGED] =
    g_signal_new ("tool-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_BRUSH_SIZE_CHANGED] =
    g_signal_new ("brush-size-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
easel_toolbox_init (EaselToolbox *self)
{
  GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());

  self->tool = EASEL_TOOL_PENCIL;
  self->brush_size = sizes[0];

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 12);
  gtk_widget_add_css_class (GTK_WIDGET (self), "easel-toolbox");
  gtk_widget_set_margin_start (GTK_WIDGET (self), 6);
  gtk_widget_set_margin_end (GTK_WIDGET (self), 6);
  gtk_widget_set_margin_top (GTK_WIDGET (self), 6);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self), 6);

  /* Tool grid: one radio group, Pencil selected. */
  GtkWidget *grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 2);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 2);

  for (int i = 0; i < EASEL_N_TOOLS; i++)
    {
      GtkWidget *button = make_tool_button ((EaselTool) i, theme);
      self->tool_buttons[i] = button;
      if (i > 0)
        gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (button),
                                     GTK_TOGGLE_BUTTON (self->tool_buttons[0]));
      g_signal_connect (button, "toggled", G_CALLBACK (on_tool_toggled), self);
      gtk_grid_attach (GTK_GRID (grid), button, i % 2, i / 2, 1, 1);
    }
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->tool_buttons[EASEL_TOOL_PENCIL]), TRUE);
  gtk_box_append (GTK_BOX (self), grid);

  /* Options: stroke size. Later this becomes a stack that follows the tool. */
  GtkWidget *size_grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (size_grid), 2);
  gtk_grid_set_column_spacing (GTK_GRID (size_grid), 2);
  gtk_widget_set_tooltip_text (size_grid, "Stroke size in pixels");

  GtkWidget *first = NULL;
  for (guint i = 0; i < G_N_ELEMENTS (sizes); i++)
    {
      g_autofree char *label = g_strdup_printf ("%d", sizes[i]);
      g_autofree char *tip = g_strdup_printf ("%d px", sizes[i]);
      GtkWidget *button = gtk_toggle_button_new_with_label (label);

      gtk_widget_add_css_class (button, "flat");
      gtk_widget_add_css_class (button, "numeric");
      gtk_widget_set_tooltip_text (button, tip);
      g_object_set_data (G_OBJECT (button), "easel-size", GINT_TO_POINTER (sizes[i]));
      if (first == NULL)
        first = button;
      else
        gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (button), GTK_TOGGLE_BUTTON (first));
      g_signal_connect (button, "toggled", G_CALLBACK (on_size_toggled), self);
      gtk_grid_attach (GTK_GRID (size_grid), button, (int) (i % 2), (int) (i / 2), 1, 1);
    }
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (first), TRUE);
  gtk_box_append (GTK_BOX (self), size_grid);
}

GtkWidget *
easel_toolbox_new (void)
{
  return g_object_new (EASEL_TYPE_TOOLBOX, NULL);
}

EaselTool
easel_toolbox_get_tool (EaselToolbox *self)
{
  g_return_val_if_fail (EASEL_IS_TOOLBOX (self), EASEL_TOOL_PENCIL);
  return self->tool;
}

int
easel_toolbox_get_brush_size (EaselToolbox *self)
{
  g_return_val_if_fail (EASEL_IS_TOOLBOX (self), 1);
  return self->brush_size;
}

const char *
easel_toolbox_get_tool_hint (EaselToolbox *self)
{
  g_return_val_if_fail (EASEL_IS_TOOLBOX (self), "");
  return tools[self->tool].hint;
}
