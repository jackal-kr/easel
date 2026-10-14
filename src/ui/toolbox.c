#include "ui/toolbox.h"
#include <glib/gi18n.h>

#include "ui/config.h"
#include "ui/settings.h"


/* Size choices. Brush and eraser sizes are the classic ones; pencil sizes
 * are an Easel extension (classic Paint's pencil is always 1 px). */
static const int pencil_sizes[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
static const int eraser_sizes[] = { 4, 6, 8, 10 };
static const int circle_sizes[] = { 7, 4, 1 };
static const int other_sizes[] = { 8, 5, 2 };

static const struct { EaselBrushShape shape; const char *name; } brush_shapes[] = {
  { EASEL_BRUSH_CIRCLE, N_("Round") },
  { EASEL_BRUSH_SQUARE, N_("Square") },
  { EASEL_BRUSH_SLASH, N_("Forward diagonal") },
  { EASEL_BRUSH_BACKSLASH, N_("Backward diagonal") },
};

typedef enum { OPTION_PENCIL, OPTION_BRUSH, OPTION_ERASER, OPTION_LINE, OPTION_FILL,
               OPTION_AIRBRUSH, OPTION_ZOOM, OPTION_OPAQUE, OPTION_TEXT_OPAQUE, OPTION_ERASER_OPAQUE,
               OPTION_GRADIENT } OptionKind;

static const int airbrush_sizes[] = { 9, 16, 24 };
static const int zoom_levels[] = EASEL_MAGNIFIER_LEVELS;

static const int line_widths[] = { 1, 2, 3, 4, 5 };
static const struct { EaselFillStyle style; const char *name; } fill_styles[] = {
  { EASEL_FILL_OUTLINE, N_("Outline") },
  { EASEL_FILL_OUTLINE_AND_FILL, N_("Outline and fill") },
  { EASEL_FILL_SOLID, N_("Fill, no outline") },
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
  GtkWidget *options_rule;   /* line between the tools and their options */
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
  int             airbrush_size;
  int             zoom_level;
  char           *font;         /* Pango description for the Text tool */
  GtkWidget      *font_button;
  gboolean        opaque, text_opaque, eraser_opaque;
  GtkWidget      *style_buttons[4];
  gboolean        syncing_style, text_smooth;
  int             gradient_style; /* EaselGradientStyle */
};

G_DEFINE_FINAL_TYPE (EaselToolbox, easel_toolbox, GTK_TYPE_WIDGET)

enum { SIGNAL_TOOL_CHANGED, SIGNAL_OPTIONS_CHANGED, SIGNAL_TEXT_STYLE, N_SIGNALS };
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
    case EASEL_TOOL_AIRBRUSH:  return "airbrush";
    case EASEL_TOOL_SELECT:
    case EASEL_TOOL_FREE_FORM_SELECT: return "select";
    case EASEL_TOOL_TEXT: return "text";
    case EASEL_TOOL_GRADIENT: return "gradient";
    case EASEL_TOOL_MAGNIFIER: return "zoom";
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
  gtk_widget_set_visible (self->options_rule, !g_str_equal (options_page_for (tool), "none"));
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
    case OPTION_AIRBRUSH:
      self->airbrush_size = size;
      break;
    case OPTION_ZOOM:
      self->zoom_level = size;
      break;
    case OPTION_OPAQUE:
      self->opaque = size != 0;
      break;
    case OPTION_TEXT_OPAQUE:
      self->text_opaque = size != 0;
      break;
    case OPTION_ERASER_OPAQUE:
      self->eraser_opaque = size != 0;
      break;
    case OPTION_GRADIENT:
      self->gradient_style = size;
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

/* Little preview on each option button, drawn 2x. It calls the same
 * easel_brush_covers() the real brush uses, so it always matches. */
static void
draw_preview (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  const Preview *p = data;

  if (p->kind == OPTION_GRADIENT)
    {
      GdkRGBA c;
      gtk_widget_get_color (GTK_WIDGET (area), &c);
      double x = (width - 20) / 2.0, y = (height - 14) / 2.0;
      cairo_pattern_t *g;

      if (p->size == EASEL_GRADIENT_RADIAL)
        g = cairo_pattern_create_radial (x + 3, y + 7, 0, x + 3, y + 7, 17);
      else
        g = cairo_pattern_create_linear (x + (p->size == EASEL_GRADIENT_REFLECTED ? 10 : 0), y, x + 20, y);
      cairo_pattern_add_color_stop_rgba (g, 0, c.red, c.green, c.blue, 1.0);
      cairo_pattern_add_color_stop_rgba (g, 1, c.red, c.green, c.blue, 0.1);
      cairo_set_source (cr, g);
      cairo_rectangle (cr, x, y, 20, 14);
      cairo_fill (cr);
      cairo_pattern_destroy (g);
      return;
    }

  if (p->kind == OPTION_OPAQUE || p->kind == OPTION_TEXT_OPAQUE || p->kind == OPTION_ERASER_OPAQUE)
    {
      GdkRGBA c;
      gtk_widget_get_color (GTK_WIDGET (area), &c);
      gdk_cairo_set_source_rgba (cr, &c);
      double x = (width - 20) / 2.0 + 0.5, y = (height - 14) / 2.0 + 0.5;

      cairo_rectangle (cr, x, y, 19, 13);
      if (p->size)
        {
          /* Opaque: the selection keeps its background (shaded), shape on top. */
          cairo_save (cr);
          cairo_set_source_rgba (cr, c.red, c.green, c.blue, 0.22);
          cairo_fill_preserve (cr);
          cairo_restore (cr);
        }
      else
        {
          static const double dash[] = { 2, 2 };
          cairo_set_dash (cr, dash, 2, 0);
        }
      cairo_set_line_width (cr, 1);
      cairo_stroke (cr);
      cairo_set_dash (cr, NULL, 0, 0);
      cairo_arc (cr, x + 9.5, y + 6.5, 3.5, 0, 2 * G_PI);
      cairo_fill (cr);
      return;
    }

  if (p->kind == OPTION_AIRBRUSH)
    {
      EaselDocument *doc = easel_document_new (24, 24, EASEL_RGB (255, 255, 255));
      GdkRGBA c;
      uint32_t rng = 7;

      if (doc == NULL)
        return;
      easel_raster_spray (doc, 12, 12, p->size, EASEL_RGB (0, 0, 0), p->size * p->size / 2, &rng);
      gtk_widget_get_color (GTK_WIDGET (area), &c);
      gdk_cairo_set_source_rgba (cr, &c);
      for (int y = 0; y < 24; y++)
        for (int x = 0; x < 24; x++)
          if (easel_document_get_pixel (doc, x, y) != EASEL_RGB (255, 255, 255))
            cairo_rectangle (cr, (width - 24) / 2.0 + x, (height - 24) / 2.0 + y, 1, 1);
      cairo_fill (cr);
      easel_document_free (doc);
      return;
    }

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
  g_autofree char *tip = kind == OPTION_OPAQUE || kind == OPTION_TEXT_OPAQUE || kind == OPTION_ERASER_OPAQUE || kind == OPTION_GRADIENT
                           ? g_strdup (shape_name)
                           : kind == OPTION_ZOOM
                           ? g_strdup_printf (_("%d×"), size)
                           : kind == OPTION_FILL
                           ? g_strdup (shape_name)
                           : (shape_name != NULL ? g_strdup_printf (_("%s, %d px"), shape_name, size)
                                                 : g_strdup_printf (_("%d px"), size));

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
      g_autofree char *label = g_strdup_printf (kind == OPTION_ZOOM ? "%dx" : "%d", size);
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

static GtkWidget *build_opaque_options (EaselToolbox *self, OptionKind kind, GtkWidget *grid, int first_row);

static GtkWidget *
build_eraser_options (EaselToolbox *self)
{
  GtkWidget *grid = make_options_grid (), *leader = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (eraser_sizes); i++)
    add_option (grid, &leader,
                make_option_button (self, OPTION_ERASER, EASEL_BRUSH_SQUARE, eraser_sizes[i], NULL, TRUE, 0),
                (int) (i % 2), (int) (i / 2), eraser_sizes[i] == self->eraser_size);
  int rows = (int) G_N_ELEMENTS (eraser_sizes) / 2;
  GtkWidget *rule = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);

  gtk_widget_set_margin_top (rule, 4);
  gtk_widget_set_margin_bottom (rule, 4);
  gtk_grid_attach (GTK_GRID (grid), rule, 0, rows, 2, 1);
  return build_opaque_options (self, OPTION_ERASER_OPAQUE, grid, rows + 1);
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

/* The Opaque / Transparent pair, shared by Select, Text and Eraser. Added to
 * `grid` from `first_row` on (a new grid when NULL). */
static GtkWidget *
build_opaque_options (EaselToolbox *self, OptionKind kind, GtkWidget *grid, int first_row)
{
  GtkWidget *leader = NULL;
  const char *const select_names[] = { _("Transparent: hide background color"), _("Opaque: keep the selection's background") };
  const char *const eraser_names[] = { _("Transparent: erase to transparency"), _("Opaque: erase to the background color") };
  gboolean current = kind == OPTION_OPAQUE ? self->opaque
                   : kind == OPTION_ERASER_OPAQUE ? self->eraser_opaque : self->text_opaque;
  const char *const *names = kind == OPTION_ERASER_OPAQUE ? eraser_names : select_names;

  if (grid == NULL)
    grid = make_options_grid ();

  for (int i = 0; i < 2; i++)
    {
      int opaque = 1 - i; /* Opaque first */
      GtkWidget *b = make_option_button (self, kind, EASEL_BRUSH_CIRCLE, opaque, names[i == 0 ? 1 : 0], TRUE, 0);
      add_option (grid, &leader, b, 0, first_row + i, (opaque != 0) == current);
      /* Full width, so the pair reads the same on every page. */
      g_object_ref (b);
      gtk_grid_remove (GTK_GRID (grid), b);
      gtk_grid_attach (GTK_GRID (grid), b, 0, first_row + i, 2, 1);
      g_object_unref (b);
    }
  return grid;
}

static GtkWidget *
build_select_options (EaselToolbox *self)
{
  return build_opaque_options (self, OPTION_OPAQUE, NULL, 0);
}

static void
on_font_chosen (GObject *source, GAsyncResult *result, gpointer data)
{
  EaselToolbox *self = data;
  g_autoptr (GError) error = NULL;
  PangoFontDescription *desc = gtk_font_dialog_choose_font_finish (GTK_FONT_DIALOG (source), result, &error);

  if (desc != NULL)
    {
      g_free (self->font);
      self->font = pango_font_description_to_string (desc);
      gtk_widget_set_tooltip_text (self->font_button, self->font);
      pango_font_description_free (desc);
      g_signal_emit (self, signals[SIGNAL_OPTIONS_CHANGED], 0);
    }
  g_object_unref (self);
}

/* The font dialog fills its list a batch at a time. If it is told the current
 * font up front, GTK selects it before its row exists and, for some fonts
 * (seen with Arial and Andale Mono inside Flatpak), leaves the list blank. So
 * the dialog opens without a font and the current one is set once the list has
 * had time to load. */
static const guint font_tries[] = { 50, 200, 500, 1000, 2000 }; /* ms after opening */

static gboolean
select_current_font (gpointer data)
{
  const char *font = data;
  GListModel *windows = gtk_window_get_toplevels ();

  /* GtkFontDialog gives no access to its chooser, so the (deprecated)
   * GtkFontChooser interface of its window is the only way to set the font. */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  for (guint i = 0; i < g_list_model_get_n_items (windows); i++)
    {
      g_autoptr (GtkWindow) window = g_list_model_get_item (windows, i);

      if (GTK_IS_FONT_CHOOSER (window))
        {
          PangoFontDescription *current = pango_font_description_from_string (font);
          PangoFontDescription *shown = gtk_font_chooser_get_font_desc (GTK_FONT_CHOOSER (window));

          /* Try again later if the row was not there yet. */
          if (shown == NULL || g_strcmp0 (pango_font_description_get_family (shown),
                                          pango_font_description_get_family (current)) != 0)
            gtk_font_chooser_set_font_desc (GTK_FONT_CHOOSER (window), current);
          pango_font_description_free (current);
        }
    }
  G_GNUC_END_IGNORE_DEPRECATIONS
  return G_SOURCE_REMOVE;
}

static void
on_font_button_clicked (GtkButton *button, EaselToolbox *self)
{
  GtkFontDialog *dialog = gtk_font_dialog_new ();
  GtkWindow *parent = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)));
  /* Only inside Flatpak was the list seen going blank, so everywhere else the
   * current font is simply passed in and shows at once. */
  gboolean in_flatpak = g_file_test ("/.flatpak-info", G_FILE_TEST_EXISTS);
  PangoFontDescription *current = in_flatpak ? NULL : pango_font_description_from_string (self->font);

  gtk_font_dialog_set_title (dialog, _("Font"));
  gtk_font_dialog_choose_font (dialog, parent, current, NULL, on_font_chosen, g_object_ref (self));
  if (in_flatpak)
    {
      /* The first try is almost always enough; the later ones cover a slow
       * machine (a try does nothing once the right font is showing). */
      char *font = g_strdup (self->font);
      for (guint i = 0; i < G_N_ELEMENTS (font_tries); i++)
        g_timeout_add (font_tries[i], select_current_font, font);
      g_timeout_add_once (font_tries[G_N_ELEMENTS (font_tries) - 1] + 1000, (GSourceOnceFunc) g_free, font);
    }
  g_clear_pointer (&current, pango_font_description_free);
  g_object_unref (dialog);
}

static const guint style_flags[4] = { EASEL_TEXT_BOLD, EASEL_TEXT_ITALIC, EASEL_TEXT_UNDERLINE, EASEL_TEXT_STRIKE };

static void
on_style_toggled (GtkToggleButton *button, EaselToolbox *self)
{
  if (self->syncing_style)
    return;
  for (int i = 0; i < 4; i++)
    if (GTK_WIDGET (button) == self->style_buttons[i])
      g_signal_emit (self, signals[SIGNAL_TEXT_STYLE], 0, style_flags[i], gtk_toggle_button_get_active (button));
}

/* Shows which character styles apply at the caret or to the selection. */
void
easel_toolbox_set_text_style (EaselToolbox *self, guint flags)
{
  g_return_if_fail (EASEL_IS_TOOLBOX (self));
  self->syncing_style = TRUE;
  for (int i = 0; i < 4; i++)
    if (self->style_buttons[i] != NULL)
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->style_buttons[i]), (flags & style_flags[i]) != 0);
  self->syncing_style = FALSE;
}

static GtkWidget *
make_style_button (EaselToolbox *self, int i, const char *icon, const char *tip)
{
  GtkWidget *b = gtk_toggle_button_new ();
  GtkWidget *image = gtk_image_new_from_icon_name (icon);

  gtk_image_set_pixel_size (GTK_IMAGE (image), 16);
  gtk_widget_set_size_request (image, 24, 24);
  gtk_button_set_child (GTK_BUTTON (b), image);
  gtk_widget_add_css_class (b, "flat");
  gtk_widget_set_tooltip_text (b, tip);
  gtk_accessible_update_property (GTK_ACCESSIBLE (b), GTK_ACCESSIBLE_PROPERTY_LABEL, tip, -1);
  self->style_buttons[i] = b;
  g_signal_connect (b, "toggled", G_CALLBACK (on_style_toggled), self);
  return b;
}

static void
on_smooth_toggled (GtkToggleButton *button, EaselToolbox *self)
{
  self->text_smooth = gtk_toggle_button_get_active (button);
  g_signal_emit (self, signals[SIGNAL_OPTIONS_CHANGED], 0);
}

static GtkWidget *
build_text_options (EaselToolbox *self)
{
  GtkWidget *grid = build_opaque_options (self, OPTION_TEXT_OPAQUE, NULL, 0);
  GtkWidget *rule = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);

  gtk_widget_set_margin_top (rule, 4);
  gtk_widget_set_margin_bottom (rule, 4);
  gtk_grid_attach (GTK_GRID (grid), rule, 0, 2, 2, 1);

  gtk_grid_attach (GTK_GRID (grid), make_style_button (self, 0, "format-text-bold-symbolic", _("Bold")), 0, 3, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), make_style_button (self, 1, "format-text-italic-symbolic", _("Italic")), 1, 3, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), make_style_button (self, 2, "format-text-underline-symbolic", _("Underline")), 0, 4, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), make_style_button (self, 3, "format-text-strikethrough-symbolic", _("Strikethrough")), 1, 4, 1, 1);

  rule = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_top (rule, 4);
  gtk_widget_set_margin_bottom (rule, 4);
  gtk_grid_attach (GTK_GRID (grid), rule, 0, 5, 2, 1);

  self->font_button = gtk_button_new_with_label (_("Font…"));
  gtk_widget_set_tooltip_text (self->font_button, self->font);
  g_signal_connect (self->font_button, "clicked", G_CALLBACK (on_font_button_clicked), self);
  gtk_grid_attach (GTK_GRID (grid), self->font_button, 0, 6, 2, 1);

  GtkWidget *smooth = gtk_toggle_button_new_with_label (_("Smooth"));
  gtk_widget_set_tooltip_text (smooth, _("Smooth text edges (anti-aliasing)"));
  g_signal_connect (smooth, "toggled", G_CALLBACK (on_smooth_toggled), self);
  gtk_grid_attach (GTK_GRID (grid), smooth, 0, 7, 2, 1);
  return grid;
}

static GtkWidget *
build_airbrush_options (EaselToolbox *self)
{
  GtkWidget *grid = make_options_grid (), *leader = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (airbrush_sizes); i++)
    add_option (grid, &leader,
                make_option_button (self, OPTION_AIRBRUSH, EASEL_BRUSH_CIRCLE, airbrush_sizes[i], NULL, TRUE, 0),
                (int) (i % 2), (int) (i / 2), airbrush_sizes[i] == self->airbrush_size);
  return grid;
}

static GtkWidget *
build_gradient_options (EaselToolbox *self)
{
  GtkWidget *grid = make_options_grid (), *leader = NULL;
  const char *const names[] = { _("Linear"), _("Radial"), _("Reflected") };

  for (int i = 0; i < 3; i++)
    add_option (grid, &leader,
                make_option_button (self, OPTION_GRADIENT, EASEL_BRUSH_CIRCLE, i, names[i], TRUE, 0),
                0, i, i == self->gradient_style);
  return grid;
}

static GtkWidget *
build_zoom_options (EaselToolbox *self)
{
  GtkWidget *grid = make_options_grid (), *leader = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (zoom_levels); i++)
    add_option (grid, &leader,
                make_option_button (self, OPTION_ZOOM, EASEL_BRUSH_CIRCLE, zoom_levels[i], NULL, FALSE, 0),
                (int) (i % 2), (int) (i / 2), zoom_levels[i] == self->zoom_level);
  return grid;
}

static GtkWidget *
build_shape_options (EaselToolbox *self, int form)
{
  GtkWidget *grid = make_options_grid (), *leader = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (fill_styles); i++)
    {
      GtkWidget *b = make_option_button (self, OPTION_FILL, EASEL_BRUSH_CIRCLE, (int) fill_styles[i].style,
                                         _(fill_styles[i].name), TRUE, form);
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
                                             _(brush_shapes[row].name), TRUE, 0);
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
  const EaselToolInfo *def = easel_tool_info (id);
  GtkWidget *button = gtk_toggle_button_new ();

  if (gtk_icon_theme_has_icon (theme, def->icon))
    gtk_button_set_icon_name (GTK_BUTTON (button), def->icon);
  else
    {
      g_debug ("no icon '%s' for tool '%s', using text label", def->icon, def->name);
      gtk_button_set_label (GTK_BUTTON (button), def->abbr);
    }

  gtk_widget_add_css_class (button, "flat");
  gtk_widget_set_tooltip_text (button, _(def->name));
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _(def->name), -1);
  g_object_set_data (G_OBJECT (button), "easel-tool", GINT_TO_POINTER (id));
  return button;
}

#define MARGIN          6
#define SECTION_SPACING 12
#define GRID_SPACING    2

/* How tall the toolbox needs to be if the tool grid has `cols` columns. */
static int
needed_height (EaselToolbox *self, int cols)
{
  int button_h = 0, option_h = 0;
  int rows = (EASEL_N_TOOLS + cols - 1) / cols;
  /* Tallest options page: brush (12 cells over `cols` columns) or pencil (4 rows).
   * Computed, not measured, so the answer does not depend on the current layout. */
  int option_rows = MAX (9, (12 + cols - 1) / cols); /* the Text page is the tallest */

  gtk_widget_measure (self->tool_buttons[0], GTK_ORIENTATION_VERTICAL, -1,
                      &button_h, NULL, NULL, NULL);
  gtk_widget_measure (self->brush_buttons[0], GTK_ORIENTATION_VERTICAL, -1,
                      &option_h, NULL, NULL, NULL);
  int options_h = option_rows * option_h + (option_rows - 1) * GRID_SPACING;

  return rows * button_h + (rows - 1) * GRID_SPACING + 2 * SECTION_SPACING + 1 + options_h + 2 * MARGIN;
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

/* Two columns of tools when the window is tall enough to show everything,
 * three when it isn't (the scrollbar is the last resort on very short
 * screens). The switch happens from an idle callback, because changing the
 * layout in the middle of a layout pass makes GTK complain.
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
  signals[SIGNAL_TEXT_STYLE] =
    g_signal_new ("text-style-toggled", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_BOOLEAN);
  signals[SIGNAL_OPTIONS_CHANGED] =
    g_signal_new ("options-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
easel_toolbox_init (EaselToolbox *self)
{
  GtkIconTheme *theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());

  gtk_icon_theme_add_resource_path (theme, "/io/github/jackal_kr/Easel/icons");

  /* Classic defaults. */
  self->tool = self->previous_tool = EASEL_TOOL_PAN;
  self->pencil_size = 1;
  self->eraser_size = 8;
  self->brush_size = 4;
  self->brush_shape = EASEL_BRUSH_CIRCLE;
  self->line_width = 1;
  self->fill_style = EASEL_FILL_OUTLINE;
  self->airbrush_size = 16;
  self->zoom_level = 2;
  self->opaque = TRUE;
  self->eraser_opaque = TRUE;
  self->text_opaque = FALSE; /* text is see-through unless asked otherwise */

  /* The defaults from Preferences > Drawing win over the built-in ones above. */
  self->font = easel_pref_string ("default-font", EASEL_DEFAULT_FONT);
  self->text_opaque = easel_pref_bool ("text-opaque", FALSE);
  self->text_smooth = easel_pref_bool ("text-smooth", FALSE);
  self->line_width = easel_pref_int ("line-width", 1);
  self->zoom_level = easel_pref_int ("magnifier-zoom", 2);

  self->columns = 2;

  gtk_widget_add_css_class (GTK_WIDGET (self), "easel-toolbox");

  /* Options pages first: selecting the initial tool below switches page. */
  self->options_rule = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_visible (self->options_rule, FALSE);
  self->options_stack = gtk_stack_new ();
  gtk_stack_set_hhomogeneous (GTK_STACK (self->options_stack), TRUE);
  gtk_stack_add_named (GTK_STACK (self->options_stack), gtk_box_new (GTK_ORIENTATION_VERTICAL, 0), "none");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_pencil_options (self), "pencil");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_brush_options (self), "brush");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_eraser_options (self), "eraser");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_line_options (self), "line");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_airbrush_options (self), "airbrush");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_zoom_options (self), "zoom");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_gradient_options (self), "gradient");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_select_options (self), "select");
  gtk_stack_add_named (GTK_STACK (self->options_stack), build_text_options (self), "text");
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
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->tool_buttons[EASEL_TOOL_PAN]), TRUE);

  self->tool_grid = grid;

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, SECTION_SPACING);
  gtk_widget_set_margin_start (content, MARGIN);
  gtk_widget_set_margin_end (content, MARGIN);
  gtk_widget_set_margin_top (content, MARGIN);
  gtk_widget_set_margin_bottom (content, MARGIN);
  gtk_box_append (GTK_BOX (content), grid);
  gtk_box_append (GTK_BOX (content), self->options_rule);
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
  return _(easel_tool_info (self->tool)->hint);
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
    case EASEL_TOOL_AIRBRUSH: return self->airbrush_size;
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

int
easel_toolbox_get_zoom_level (EaselToolbox *self)
{
  g_return_val_if_fail (EASEL_IS_TOOLBOX (self), 2);
  return self->zoom_level;
}

gboolean
easel_toolbox_get_opaque (EaselToolbox *self)
{
  g_return_val_if_fail (EASEL_IS_TOOLBOX (self), TRUE);
  return self->opaque;
}

/* The font family and size chosen in the dialog (a Pango description). */
const char *
easel_toolbox_get_font (EaselToolbox *self)
{
  g_return_val_if_fail (EASEL_IS_TOOLBOX (self), "Sans 12");
  return self->font;
}

int
easel_toolbox_get_gradient_style (EaselToolbox *self)
{
  g_return_val_if_fail (EASEL_IS_TOOLBOX (self), 0);
  return self->gradient_style;
}

gboolean
easel_toolbox_get_text_smooth (EaselToolbox *self)
{
  g_return_val_if_fail (EASEL_IS_TOOLBOX (self), FALSE);
  return self->text_smooth;
}

gboolean
easel_toolbox_get_eraser_opaque (EaselToolbox *self)
{
  g_return_val_if_fail (EASEL_IS_TOOLBOX (self), TRUE);
  return self->eraser_opaque;
}

gboolean
easel_toolbox_get_text_opaque (EaselToolbox *self)
{
  g_return_val_if_fail (EASEL_IS_TOOLBOX (self), FALSE);
  return self->text_opaque;
}
