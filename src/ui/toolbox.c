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
    "Erases a portion of the picture. Right button: replaces the foreground color with the background color.", TRUE },
  [EASEL_TOOL_FILL] = { "Fill With Color", "color-fill-symbolic", "Fl",
    "Fills an area with the foreground color (left button) or the background color (right button).", TRUE },
  [EASEL_TOOL_PICK_COLOR] = { "Pick Color", "color-select-symbolic", "Pk",
    "Picks up a color from the picture for drawing. Left button: foreground. Right button: background.", TRUE },
  [EASEL_TOOL_MAGNIFIER] = { "Magnifier", "zoom-in-symbolic", "Mg",
    "Changes the magnification.", FALSE },
  [EASEL_TOOL_PENCIL] = { "Pencil", "document-edit-symbolic", "Pe",
    "Draws a free-form line. Left button: foreground color. Right button: background color.", TRUE },
  [EASEL_TOOL_BRUSH] = { "Brush", "applications-graphics-symbolic", "Br",
    "Draws using a brush with the selected shape and size.", TRUE },
  [EASEL_TOOL_AIRBRUSH] = { "Airbrush", "weather-showers-symbolic", "Ai",
    "Draws using an airbrush of the selected size.", FALSE },
  [EASEL_TOOL_TEXT] = { "Text", "insert-text-symbolic", "Tx",
    "Inserts text into the picture.", FALSE },
  [EASEL_TOOL_LINE] = { "Line", "list-remove-symbolic", "Ln",
    "Draws a straight line with the selected line width.", TRUE },
  [EASEL_TOOL_CURVE] = { "Curve", "media-playlist-shuffle-symbolic", "Cv",
    "Draws a curved line with the selected line width.", FALSE },
  [EASEL_TOOL_RECTANGLE] = { "Rectangle", "checkbox-symbolic", "Re",
    "Draws a rectangle with the selected fill style.", TRUE },
  [EASEL_TOOL_POLYGON] = { "Polygon", "emblem-shared-symbolic", "Po",
    "Draws a polygon with the selected fill style.", FALSE },
  [EASEL_TOOL_ELLIPSE] = { "Ellipse", "media-record-symbolic", "El",
    "Draws an ellipse with the selected fill style.", TRUE },
  [EASEL_TOOL_ROUNDED_RECTANGLE] = { "Rounded Rectangle", "checkbox-symbolic", "RR",
    "Draws a rounded rectangle with the selected fill style.", TRUE },
};

/* Size choices. Brush and eraser sizes are the classic ones; pencil sizes
 * are an Easel extension (classic Paint's pencil is always 1 px). */
static const int pencil_sizes[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
static const int eraser_sizes[] = { 4, 6, 8, 10 };
static const int circle_sizes[] = { 7, 4, 1 };
static const int other_sizes[] = { 8, 5, 2 };

static const struct { EaselBrushShape shape; const char *name; } brush_shapes[] = {
  { EASEL_BRUSH_CIRCLE, "Round" },
  { EASEL_BRUSH_SQUARE, "Square" },
  { EASEL_BRUSH_SLASH, "Forward diagonal" },
  { EASEL_BRUSH_BACKSLASH, "Backward diagonal" },
};

typedef enum { OPTION_PENCIL, OPTION_BRUSH, OPTION_ERASER, OPTION_LINE, OPTION_FILL } OptionKind;

static const int line_widths[] = { 1, 2, 3, 4, 5 };
static const struct { EaselFillStyle style; const char *name; } fill_styles[] = {
  { EASEL_FILL_OUTLINE, "Outline" },
  { EASEL_FILL_OUTLINE_AND_FILL, "Outline and fill" },
  { EASEL_FILL_SOLID, "Fill, no outline" },
};

struct _EaselToolbox
{
  GtkWidget parent_instance;

  GtkWidget *scroller;
  GtkWidget *tool_buttons[EASEL_N_TOOLS];
  GtkWidget *tool_grid;
  GtkWidget *brush_grid;
  GtkWidget *brush_buttons[12];
  GtkWidget *fill_buttons[4][3]; /* per shape page; all pages share one fill style */
  GtkWidget *options_stack;
  int        columns;           /* tool grid columns: 2, or 3 in short windows */
  gboolean   relayout_pending;

  EaselTool       tool;
  EaselTool       previous_tool;
  int             pencil_size;
  int             eraser_size;
  int             brush_size;
  EaselBrushShape brush_shape;
  int             line_width;
  EaselFillStyle  fill_style;
};

G_DEFINE_FINAL_TYPE (EaselToolbox, easel_toolbox, GTK_TYPE_WIDGET)

enum { SIGNAL_TOOL_CHANGED, SIGNAL_OPTIONS_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

static const char *
options_page_for (EaselTool tool)
{
  switch (tool)
    {
    case EASEL_TOOL_PENCIL: return "pencil";
    case EASEL_TOOL_BRUSH:  return "brush";
    case EASEL_TOOL_ERASER: return "eraser";
    case EASEL_TOOL_LINE:
    case EASEL_TOOL_CURVE:  return "line";
    case EASEL_TOOL_RECTANGLE:         return "shape-rect";
    case EASEL_TOOL_ROUNDED_RECTANGLE: return "shape-rounded";
    case EASEL_TOOL_ELLIPSE:           return "shape-ellipse";
    case EASEL_TOOL_POLYGON:           return "shape-polygon";
    default:                return "none";
    }
}

static void
on_tool_toggled (GtkToggleButton *button, EaselToolbox *self)
{
  if (!gtk_toggle_button_get_active (button))
    return;

  EaselTool tool = (EaselTool) GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "easel-tool"));
  if (tool != self->tool)
    {
      self->previous_tool = self->tool;
      self->tool = tool;
    }

  gtk_stack_set_visible_child_name (GTK_STACK (self->options_stack), options_page_for (tool));
  g_signal_emit (self, signals[SIGNAL_TOOL_CHANGED], 0);
}

static void
on_option_toggled (GtkToggleButton *button, EaselToolbox *self)
{
  if (!gtk_toggle_button_get_active (button))
    return;

  OptionKind kind = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "easel-kind"));
  int size = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "easel-size"));
  EaselBrushShape shape = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "easel-shape"));

  switch (kind)
    {
    case OPTION_PENCIL:
      self->pencil_size = size;
      break;
    case OPTION_ERASER:
      self->eraser_size = size;
      break;
    case OPTION_BRUSH:
      self->brush_size = size;
      self->brush_shape = shape;
      break;
    case OPTION_LINE:
      self->line_width = size;
      break;
    case OPTION_FILL:
      self->fill_style = (EaselFillStyle) size;
      /* Keep the other shape pages showing the same (shared) choice. */
      for (int f = 0; f < 4; f++)
        for (guint i = 0; i < G_N_ELEMENTS (fill_styles); i++)
          if (fill_styles[i].style == self->fill_style && self->fill_buttons[f][i] != NULL)
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->fill_buttons[f][i]), TRUE);
      break;
    }
  g_signal_emit (self, signals[SIGNAL_OPTIONS_CHANGED], 0);
}

typedef struct
{
  OptionKind      kind;
  EaselBrushShape shape;
  int             size; /* pixels, or the EaselFillStyle for OPTION_FILL */
  int             form; /* OPTION_FILL: 0 rectangle, 1 rounded, 2 ellipse, 3 polygon */
} Preview;

/* Draws what the brush footprint looks like, 2x magnified, using the real
 * engine so the preview can never disagree with what gets drawn. */
static void
draw_preview (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  const Preview *p = data;

  if (p->kind == OPTION_LINE || p->kind == OPTION_FILL)
    {
      GdkRGBA c;
      gtk_widget_get_color (GTK_WIDGET (area), &c);
      gdk_cairo_set_source_rgba (cr, &c);
      if (p->kind == OPTION_LINE)
        {
          cairo_rectangle (cr, 2, (height - p->size) / 2.0, width - 4, p->size);
          cairo_fill (cr);
        }
      else
        {
          double x = 3.5, y = 5.5, w = width - 7, h = height - 11;
          switch (p->form)
            {
            case 1:
              cairo_new_sub_path (cr);
              cairo_arc (cr, x + w - 3, y + 3, 3, -G_PI / 2, 0);
              cairo_arc (cr, x + w - 3, y + h - 3, 3, 0, G_PI / 2);
              cairo_arc (cr, x + 3, y + h - 3, 3, G_PI / 2, G_PI);
              cairo_arc (cr, x + 3, y + 3, 3, G_PI, 3 * G_PI / 2);
              cairo_close_path (cr);
              break;
            case 2:
              cairo_save (cr);
              cairo_translate (cr, x + w / 2, y + h / 2);
              cairo_scale (cr, w / 2, h / 2);
              cairo_arc (cr, 0, 0, 1, 0, 2 * G_PI);
              cairo_restore (cr);
              break;
            case 3:
              cairo_move_to (cr, x + 1, y + h);
              cairo_line_to (cr, x, y + h * 0.35);
              cairo_line_to (cr, x + w * 0.55, y);
              cairo_line_to (cr, x + w, y + h * 0.5);
              cairo_line_to (cr, x + w * 0.7, y + h);
              cairo_close_path (cr);
              break;
            default:
              cairo_rectangle (cr, x, y, w, h);
              break;
            }
          if (p->size == EASEL_FILL_SOLID)
            cairo_fill (cr);
          else
            {
              if (p->size == EASEL_FILL_OUTLINE_AND_FILL)
                {
                  cairo_fill_preserve (cr);
                  cairo_set_source_rgba (cr, 1, 1, 1, 0.65);
                  cairo_fill_preserve (cr);
                  gdk_cairo_set_source_rgba (cr, &c);
                }
              cairo_set_line_width (cr, 1.5);
              cairo_stroke (cr);
            }
        }
      return;
    }

  enum { CELLS = 12, ZOOM = 2 };
  EaselDocument *doc = easel_document_new (CELLS, CELLS, EASEL_RGB (255, 255, 255));
  GdkRGBA color;

  if (doc == NULL)
    return;

  EaselBrush brush = { p->shape, p->size, EASEL_RGB (0, 0, 0) };
  easel_raster_stamp (doc, CELLS / 2, CELLS / 2, &brush);

  gtk_widget_get_color (GTK_WIDGET (area), &color);
  gdk_cairo_set_source_rgba (cr, &color);

  double ox = (width - CELLS * ZOOM) / 2.0;
  double oy = (height - CELLS * ZOOM) / 2.0;
  for (int y = 0; y < CELLS; y++)
    for (int x = 0; x < CELLS; x++)
      if (easel_document_get_pixel (doc, x, y) != EASEL_RGB (255, 255, 255))
        cairo_rectangle (cr, ox + x * ZOOM, oy + y * ZOOM, ZOOM, ZOOM);
  cairo_fill (cr);

  easel_document_free (doc);
}

static GtkWidget *
make_option_button (EaselToolbox *self, OptionKind kind, EaselBrushShape shape, int size,
                    const char *shape_name, gboolean with_preview, int form)
{
  GtkWidget *button = gtk_toggle_button_new ();
  g_autofree char *tip = kind == OPTION_FILL
                           ? g_strdup (shape_name)
                           : (shape_name != NULL ? g_strdup_printf ("%s, %d px", shape_name, size)
                                                 : g_strdup_printf ("%d px", size));

  if (with_preview)
    {
      Preview *preview = g_new (Preview, 1);
      preview->kind = kind;
      preview->form = form;
      preview->shape = shape;
      preview->size = size;

      GtkWidget *area = gtk_drawing_area_new ();
      gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (area), 24);
      gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (area), 24);
      gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (area), draw_preview, preview, g_free);
      gtk_button_set_child (GTK_BUTTON (button), area);
    }
  else
    {
      g_autofree char *label = g_strdup_printf ("%d", size);
      gtk_button_set_label (GTK_BUTTON (button), label);
      gtk_widget_add_css_class (button, "numeric");
    }

  gtk_widget_add_css_class (button, "flat");
  gtk_widget_set_tooltip_text (button, tip);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, tip, -1);
  g_object_set_data (G_OBJECT (button), "easel-kind", GINT_TO_POINTER (kind));
  g_object_set_data (G_OBJECT (button), "easel-size", GINT_TO_POINTER (size));
  g_object_set_data (G_OBJECT (button), "easel-shape", GINT_TO_POINTER (shape));
  g_signal_connect (button, "toggled", G_CALLBACK (on_option_toggled), self);
  return button;
}

static GtkWidget *
make_options_grid (void)
{
  GtkWidget *grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 2);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 2);
  gtk_grid_set_column_homogeneous (GTK_GRID (grid), TRUE);
  return grid;
}

/* Adds `button` to `grid` and into the radio group led by `*leader`. */
static void
add_option (GtkWidget *grid, GtkWidget **leader, GtkWidget *button, int col, int row, gboolean active)
{
  if (*leader == NULL)
    *leader = button;
  else
    gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (button), GTK_TOGGLE_BUTTON (*leader));
  gtk_grid_attach (GTK_GRID (grid), button, col, row, 1, 1);
  if (active)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (button), TRUE);
}

static GtkWidget *
build_pencil_options (EaselToolbox *self)
{
  GtkWidget *grid = make_options_grid (), *leader = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (pencil_sizes); i++)
    add_option (grid, &leader,
                make_option_button (self, OPTION_PENCIL, EASEL_BRUSH_CIRCLE, pencil_sizes[i], NULL, FALSE, 0),
                (int) (i % 2), (int) (i / 2), pencil_sizes[i] == self->pencil_size);
  return grid;
}

static GtkWidget *
build_eraser_options (EaselToolbox *self)
{
  GtkWidget *grid = make_options_grid (), *leader = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (eraser_sizes); i++)
    add_option (grid, &leader,
                make_option_button (self, OPTION_ERASER, EASEL_BRUSH_SQUARE, eraser_sizes[i], NULL, TRUE, 0),
                (int) (i % 2), (int) (i / 2), eraser_sizes[i] == self->eraser_size);
  return grid;
}

static GtkWidget *
build_line_options (EaselToolbox *self)
{
  GtkWidget *grid = make_options_grid (), *leader = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (line_widths); i++)
    add_option (grid, &leader,
                make_option_button (self, OPTION_LINE, EASEL_BRUSH_CIRCLE, line_widths[i], NULL, TRUE, 0),
                (int) (i % 2), (int) (i / 2), line_widths[i] == self->line_width);
  return grid;
}

static GtkWidget *
build_shape_options (EaselToolbox *self, int form)
{
  GtkWidget *grid = make_options_grid (), *leader = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (fill_styles); i++)
    {
      GtkWidget *b = make_option_button (self, OPTION_FILL, EASEL_BRUSH_CIRCLE, (int) fill_styles[i].style,
                                         fill_styles[i].name, TRUE, form);
      self->fill_buttons[form][i] = b;
      add_option (grid, &leader, b, (int) (i % 2), (int) (i / 2), fill_styles[i].style == self->fill_style);
    }
  return grid;
}

/* One row per shape, three sizes per row, like the classic 12-cell chooser. */
static GtkWidget *
build_brush_options (EaselToolbox *self)
{
  GtkWidget *grid = make_options_grid (), *leader = NULL;

  self->brush_grid = grid;
  for (guint row = 0; row < G_N_ELEMENTS (brush_shapes); row++)
    {
      EaselBrushShape shape = brush_shapes[row].shape;
      const int *sizes = shape == EASEL_BRUSH_CIRCLE ? circle_sizes : other_sizes;

      for (int col = 0; col < 3; col++)
        {
          int n = (int) row * 3 + col;
          GtkWidget *b = make_option_button (self, OPTION_BRUSH, shape, sizes[col],
                                             brush_shapes[row].name, TRUE, 0);
          self->brush_buttons[n] = b;
          add_option (grid, &leader, b, n % 2, n / 2,
                      shape == self->brush_shape && sizes[col] == self->brush_size);
        }
    }
  return grid;
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

#define MARGIN          6
#define SECTION_SPACING 12
#define GRID_SPACING    2

/* Height the content needs with the tool grid in `cols` columns. */
static int
needed_height (EaselToolbox *self, int cols)
{
  int button_h = 0, option_h = 0;
  int rows = (EASEL_N_TOOLS + cols - 1) / cols;
  /* Tallest options page: brush (12 cells over `cols` columns) or pencil (4 rows).
   * Computed, not measured, so the answer does not depend on the current layout. */
  int option_rows = MAX (5, (12 + cols - 1) / cols);

  gtk_widget_measure (self->tool_buttons[0], GTK_ORIENTATION_VERTICAL, -1,
                      &button_h, NULL, NULL, NULL);
  gtk_widget_measure (self->brush_buttons[0], GTK_ORIENTATION_VERTICAL, -1,
                      &option_h, NULL, NULL, NULL);
  int options_h = option_rows * option_h + (option_rows - 1) * GRID_SPACING;

  return rows * button_h + (rows - 1) * GRID_SPACING + SECTION_SPACING + options_h + 2 * MARGIN;
}

static void
apply_columns (EaselToolbox *self, int cols)
{
  GtkLayoutManager *manager = gtk_widget_get_layout_manager (self->tool_grid);

  for (int i = 0; i < EASEL_N_TOOLS; i++)
    {
      GtkGridLayoutChild *child = GTK_GRID_LAYOUT_CHILD (
        gtk_layout_manager_get_layout_child (manager, self->tool_buttons[i]));
      gtk_grid_layout_child_set_column (child, i % cols);
      gtk_grid_layout_child_set_row (child, i / cols);
    }

  /* Brush options follow the tool columns (flowing, 12 cells). */
  GtkLayoutManager *bm = gtk_widget_get_layout_manager (self->brush_grid);
  for (int i = 0; i < 12; i++)
    {
      GtkGridLayoutChild *child = GTK_GRID_LAYOUT_CHILD (
        gtk_layout_manager_get_layout_child (bm, self->brush_buttons[i]));
      gtk_grid_layout_child_set_column (child, i % cols);
      gtk_grid_layout_child_set_row (child, i / cols);
    }
  self->columns = cols;
}

static void
relayout_idle (gpointer data)
{
  EaselToolbox *self = data;
  int height = gtk_widget_get_height (GTK_WIDGET (self));
  int cols = height >= needed_height (self, 2) ? 2 : 3;

  self->relayout_pending = FALSE;
  if (cols != self->columns)
    apply_columns (self, cols);
  g_object_unref (self);
}

static void
easel_toolbox_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                       int *minimum, int *natural, int *minimum_baseline, int *natural_baseline)
{
  EaselToolbox *self = EASEL_TOOLBOX (widget);
  gtk_widget_measure (self->scroller, orientation, for_size,
                      minimum, natural, minimum_baseline, natural_baseline);
}

/* Two columns when the window is tall enough to show everything, three when
 * it is not (the scroller is only the last resort for very short windows).
 * The change is deferred to an idle so we never re-layout inside allocation.
 * (This widget does its own layout: a GtkBox's layout manager would bypass
 * this vfunc.) */
static void
easel_toolbox_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  EaselToolbox *self = EASEL_TOOLBOX (widget);

  gtk_widget_allocate (self->scroller, width, height, baseline, NULL);

  int wanted = height >= needed_height (self, 2) ? 2 : 3;
  if (wanted != self->columns && !self->relayout_pending)
    {
      self->relayout_pending = TRUE;
      g_idle_add_once (relayout_idle, g_object_ref (self));
    }
}

static void
easel_toolbox_dispose (GObject *object)
{
  EaselToolbox *self = EASEL_TOOLBOX (object);
  g_clear_pointer (&self->scroller, gtk_widget_unparent);
  G_OBJECT_CLASS (easel_toolbox_parent_class)->dispose (object);
}

static void
easel_toolbox_class_init (EaselToolboxClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = easel_toolbox_dispose;
  GTK_WIDGET_CLASS (klass)->measure = easel_toolbox_measure;
  GTK_WIDGET_CLASS (klass)->size_allocate = easel_toolbox_size_allocate;

  signals[SIGNAL_TOOL_CHANGED] =
    g_signal_new ("tool-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[SIGNAL_OPTIONS_CHANGED] =
    g_signal_new ("options-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
easel_toolbox_init (EaselToolbox *self)
{
  GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());

  /* Classic defaults. */
  self->tool = self->previous_tool = EASEL_TOOL_PENCIL;
  self->pencil_size = 1;
  self->eraser_size = 8;
  self->brush_size = 4;
  self->brush_shape = EASEL_BRUSH_CIRCLE;
  self->line_width = 1;
  self->fill_style = EASEL_FILL_OUTLINE;

  self->columns = 2;

  gtk_widget_add_css_class (GTK_WIDGET (self), "easel-toolbox");

  /* Options pages first: selecting the initial tool below switches page. */
  self->options_stack = gtk_stack_new ();
  gtk_stack_set_hhomogeneous (GTK_STACK (self->options_stack), TRUE);
  gtk_stack_add_named (GTK_STACK (self->options_stack), gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), "none");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_pencil_options (self), "pencil");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_brush_options (self), "brush");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_eraser_options (self), "eraser");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_line_options (self), "line");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_shape_options (self, 0), "shape-rect");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_shape_options (self, 1), "shape-rounded");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_shape_options (self, 2), "shape-ellipse");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_shape_options (self, 3), "shape-polygon");

  /* Tool grid: one radio group, Pencil selected. */
  GtkWidget *grid = make_options_grid ();
  gtk_grid_set_column_homogeneous (GTK_GRID (grid), TRUE);
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

  self->tool_grid = grid;

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, SECTION_SPACING);
  gtk_widget_set_margin_start (content, MARGIN);
  gtk_widget_set_margin_end (content, MARGIN);
  gtk_widget_set_margin_top (content, MARGIN);
  gtk_widget_set_margin_bottom (content, MARGIN);
  gtk_box_append (GTK_BOX (content), grid);
  gtk_box_append (GTK_BOX (content), self->options_stack);

  /* Never cut anything off: scroll vertically if even three columns do not fit. */
  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width (GTK_SCROLLED_WINDOW (scroller), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), content);
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_widget_set_parent (scroller, GTK_WIDGET (self));
  self->scroller = scroller;
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

const char *
easel_toolbox_get_tool_hint (EaselToolbox *self)
{
  g_return_val_if_fail (EASEL_IS_TOOLBOX (self), "");
  return tools[self->tool].hint;
}

int
easel_toolbox_get_tool_size (EaselToolbox *self)
{
  g_return_val_if_fail (EASEL_IS_TOOLBOX (self), 1);

  switch (self->tool)
    {
    case EASEL_TOOL_PENCIL: return self->pencil_size;
    case EASEL_TOOL_BRUSH:  return self->brush_size;
    case EASEL_TOOL_ERASER: return self->eraser_size;
    default:                return 1;
    }
}

EaselBrushShape
easel_toolbox_get_brush_shape (EaselToolbox *self)
{
  g_return_val_if_fail (EASEL_IS_TOOLBOX (self), EASEL_BRUSH_CIRCLE);
  return self->brush_shape;
}

void
easel_toolbox_select_tool (EaselToolbox *self, EaselTool tool)
{
  g_return_if_fail (EASEL_IS_TOOLBOX (self));
  g_return_if_fail (tool >= 0 && tool < EASEL_N_TOOLS);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->tool_buttons[tool]), TRUE);
}

void
easel_toolbox_select_previous_tool (EaselToolbox *self)
{
  g_return_if_fail (EASEL_IS_TOOLBOX (self));
  easel_toolbox_select_tool (self, self->previous_tool);
}

int
easel_toolbox_get_line_width (EaselToolbox *self)
{
  g_return_val_if_fail (EASEL_IS_TOOLBOX (self), 1);
  return self->line_width;
}

EaselFillStyle
easel_toolbox_get_fill_style (EaselToolbox *self)
{
  g_return_val_if_fail (EASEL_IS_TOOLBOX (self), EASEL_FILL_OUTLINE);
  return self->fill_style;
}
