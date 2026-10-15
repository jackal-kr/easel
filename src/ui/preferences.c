#include "ui/preferences.h"

#include <adwaita.h>
#include <glib/gi18n.h>

#include "ui/config.h"
#include "ui/settings.h"

/* ---- Row helpers ------------------------------------------------------ */

static AdwSwitchRow *
add_switch (AdwPreferencesGroup *group, GSettings *s, const char *key, const char *title, const char *subtitle)
{
  GtkWidget *row = adw_switch_row_new ();

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle != NULL)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  g_settings_bind (s, key, row, "active", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (group, row);
  return ADW_SWITCH_ROW (row);
}

static void
add_spin (AdwPreferencesGroup *group, GSettings *s, const char *key, const char *title,
          const char *subtitle, double lo, double hi)
{
  GtkWidget *row = adw_spin_row_new_with_range (lo, hi, 1);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle != NULL)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  g_settings_bind (s, key, adw_spin_row_get_adjustment (ADW_SPIN_ROW (row)), "value", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (group, row);
}

/* A combo row stored as one of a list of strings. */
typedef struct
{
  GSettings          *settings;
  const char         *key;
  const char *const  *values;
} ChoiceData;

static void
on_choice_selected (AdwComboRow *row, GParamSpec *pspec, ChoiceData *data)
{
  g_settings_set_string (data->settings, data->key, data->values[adw_combo_row_get_selected (row)]);
}

static void
add_choice (AdwPreferencesGroup *group, GSettings *s, const char *key, const char *title,
            const char *const *labels, const char *const *values)
{
  GtkWidget *row = adw_combo_row_new ();
  g_autofree char *current = g_settings_get_string (s, key);
  ChoiceData *data = g_new0 (ChoiceData, 1);
  guint selected = 0;

  data->settings = s;
  data->key = key;
  data->values = values;
  for (guint i = 0; values[i] != NULL; i++)
    if (g_str_equal (values[i], current))
      selected = i;

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (gtk_string_list_new (labels)));
  adw_combo_row_set_selected (ADW_COMBO_ROW (row), selected);
  g_signal_connect (row, "notify::selected", G_CALLBACK (on_choice_selected), data);
  g_object_set_data_full (G_OBJECT (row), "choice-data", data, g_free);
  adw_preferences_group_add (group, row);
}

/* Integer choice (the Magnifier zoom). */
static void
on_int_choice_selected (AdwComboRow *row, GParamSpec *pspec, GSettings *s)
{
  static const int values[] = EASEL_MAGNIFIER_LEVELS;
  g_settings_set_int (s, "magnifier-zoom", values[adw_combo_row_get_selected (row)]);
}

/* ---- Color buttons ---------------------------------------------------- */

typedef struct
{
  GSettings *settings;
  const char *key;
  GtkWidget *button;
  uint32_t   fallback;
} ColorData;

static void
on_color_set (GObject *button, GParamSpec *pspec, ColorData *data)
{
  const GdkRGBA *c = gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (button));
  g_autofree char *hex = easel_argb_to_hex (easel_rgba_to_argb (c));
  g_settings_set_string (data->settings, data->key, hex);
}

static GtkWidget *
make_color_button (GSettings *s, const char *key, uint32_t fallback, ColorData **out)
{
  g_autofree char *hex = g_settings_get_string (s, key);
  uint32_t argb;
  ColorData *data = g_new0 (ColorData, 1);
  g_autoptr (GtkColorDialog) dialog = gtk_color_dialog_new ();
  GtkWidget *button = gtk_color_dialog_button_new (g_object_ref (dialog));
  GdkRGBA rgba;

  if (!easel_parse_hex_color (hex, &argb))
    argb = fallback;
  rgba = easel_argb_to_rgba (argb, 1);
  gtk_color_dialog_set_with_alpha (dialog, FALSE);
  gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (button), &rgba);
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);

  data->settings = s;
  data->key = key;
  data->button = button;
  data->fallback = fallback;
  g_signal_connect (button, "notify::rgba", G_CALLBACK (on_color_set), data);
  g_object_set_data_full (G_OBJECT (button), "color-data", data, g_free);
  *out = data;
  return button;
}

static void
on_workspace_reset (GtkButton *button, GSettings *s)
{
  g_settings_reset (s, "workspace-color");
}

/* ---- Pages ------------------------------------------------------------- */

static AdwPreferencesPage *
make_page (const char *title, const char *icon)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());

  adw_preferences_page_set_title (page, title);
  adw_preferences_page_set_icon_name (page, icon);
  return page;
}

static AdwPreferencesGroup *
add_group (AdwPreferencesPage *page, const char *title, const char *description)
{
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

  if (title != NULL)
    adw_preferences_group_set_title (group, title);
  if (description != NULL)
    adw_preferences_group_set_description (group, description);
  adw_preferences_page_add (page, group);
  return group;
}

static void
build_appearance (AdwPreferencesDialog *dialog, GSettings *s)
{
  AdwPreferencesPage *page = make_page (_("Appearance"), "applications-graphics-symbolic");
  AdwPreferencesGroup *style = add_group (page, _("Style"), NULL);
  const char *const labels[] = { _("Follow system"), _("Light"), _("Dark"), NULL };
  static const char *const values[] = { "system", "light", "dark", NULL };

  add_choice (style, s, "color-scheme", _("Color scheme"), labels, values);

  GtkWidget *row = adw_action_row_new ();
  ColorData *cd;
  GtkWidget *reset = gtk_button_new_from_icon_name ("edit-undo-symbolic");
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _("Workspace color"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), _("The area around the picture. Reset follows the theme."));
  gtk_widget_add_css_class (reset, "flat");
  gtk_widget_set_valign (reset, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (reset, _("Use the theme color"));
  g_signal_connect (reset, "clicked", G_CALLBACK (on_workspace_reset), s);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), reset);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), make_color_button (s, "workspace-color", 0xFFE6E6E6, &cd));
  adw_preferences_group_add (style, row);

  AdwPreferencesGroup *panels = add_group (page, _("Panels"), _("Which parts of the window are shown. Also changed from the View menu."));
  add_switch (panels, s, "show-toolbox", _("Tool box"), NULL);
  add_switch (panels, s, "show-colorbox", _("Color box"), NULL);
  add_switch (panels, s, "show-statusbar", _("Status bar"), NULL);
  add_switch (panels, s, "show-frame-time", _("Frame time"), _("Shown in the status bar"));
  adw_preferences_dialog_add (dialog, page);
}

static void
build_new_picture (AdwPreferencesDialog *dialog, GSettings *s)
{
  AdwPreferencesPage *page = make_page (_("New Picture"), "document-new-symbolic");
  AdwPreferencesGroup *group = add_group (page, _("Default picture"), _("Used when Easel starts and for File → New."));
  ColorData *cd;

  add_spin (group, s, "default-width", _("Width"), "pixels", 1, EASEL_MAX_DIMENSION);
  add_spin (group, s, "default-height", _("Height"), "pixels", 1, EASEL_MAX_DIMENSION);

  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _("Background color"));
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), make_color_button (s, "default-background", 0xFFFFFFFF, &cd));
  adw_preferences_group_add (group, row);
  adw_preferences_dialog_add (dialog, page);
}

static void
on_font_set (GObject *button, GParamSpec *pspec, GSettings *s)
{
  PangoFontDescription *desc = gtk_font_dialog_button_get_font_desc (GTK_FONT_DIALOG_BUTTON (button));
  g_autofree char *text = desc != NULL ? pango_font_description_to_string (desc) : NULL;

  if (text != NULL)
    g_settings_set_string (s, "default-font", text);
}

static void
build_drawing (AdwPreferencesDialog *dialog, GSettings *s)
{
  AdwPreferencesPage *page = make_page (_("Drawing"), "document-edit-symbolic");
  AdwPreferencesGroup *tools = add_group (page, _("Tool defaults"), _("Applied the next time Easel starts."));

  add_spin (tools, s, "line-width", _("Line width"), _("Line, Curve and shape tools"), 1, 5);

  GtkWidget *zoom = adw_combo_row_new ();
  static const char *const zoom_labels[] = { "1×", "2×", "4×", "6×", "8×", NULL };
  static const int zoom_values[] = EASEL_MAGNIFIER_LEVELS;
  int current = g_settings_get_int (s, "magnifier-zoom");
  guint selected = 0;

  for (guint i = 0; i < G_N_ELEMENTS (zoom_values); i++)
    if (zoom_values[i] == current)
      selected = i;
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (zoom), _("Magnifier click"));
  adw_combo_row_set_model (ADW_COMBO_ROW (zoom), G_LIST_MODEL (gtk_string_list_new (zoom_labels)));
  adw_combo_row_set_selected (ADW_COMBO_ROW (zoom), selected);
  g_signal_connect (zoom, "notify::selected", G_CALLBACK (on_int_choice_selected), s);
  adw_preferences_group_add (tools, zoom);

  AdwPreferencesGroup *undo = add_group (page, _("Undo"), _("Applied the next time Easel starts."));
  add_spin (undo, s, "undo-limit-mb", _("Undo history size"), _("Megabytes of history kept (the newest 64 MB in memory, the rest on disk); older steps are forgotten first"), 16, 32768);

  AdwPreferencesGroup *text = add_group (page, _("Text"), _("Applied the next time Easel starts."));
  GtkWidget *row = adw_action_row_new ();
  g_autofree char *font = g_settings_get_string (s, "default-font");
  g_autoptr (GtkFontDialog) dialog_font = gtk_font_dialog_new ();
  GtkWidget *button = gtk_font_dialog_button_new (g_object_ref (dialog_font));
  PangoFontDescription *desc = pango_font_description_from_string (font);

  gtk_font_dialog_button_set_font_desc (GTK_FONT_DIALOG_BUTTON (button), desc);
  pango_font_description_free (desc);
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  g_signal_connect (button, "notify::font-desc", G_CALLBACK (on_font_set), s);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _("Font"));
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), button);
  adw_preferences_group_add (text, row);
  add_switch (text, s, "text-opaque", _("Opaque background"), _("Otherwise the picture shows through the text"));
  add_switch (text, s, "text-smooth", _("Smooth edges"), _("Antialiased text instead of hard pixels"));
  adw_preferences_dialog_add (dialog, page);
}

static void
on_palette_reset (GtkButton *button, GSettings *s)
{
  g_settings_reset (s, "palette");
}

static void
build_palette (AdwPreferencesDialog *dialog, GSettings *s)
{
  AdwPreferencesPage *page = make_page (_("Palette"), "color-select-symbolic");
  AdwPreferencesGroup *group = add_group (page, _("Palette"),
                                          _("Colors you change with Edit Colors are kept between runs."));
  GtkWidget *row = adw_action_row_new ();
  GtkWidget *reset = gtk_button_new_with_label (_("Reset"));

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _("Classic palette"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), _("Go back to the original 28 colors"));
  gtk_widget_set_valign (reset, GTK_ALIGN_CENTER);
  g_signal_connect (reset, "clicked", G_CALLBACK (on_palette_reset), s);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), reset);
  adw_preferences_group_add (group, row);
  adw_preferences_dialog_add (dialog, page);
}

void
easel_preferences_present (GtkWidget *parent)
{
  GSettings *s = easel_settings ();
  AdwPreferencesDialog *dialog = ADW_PREFERENCES_DIALOG (adw_preferences_dialog_new ());

  adw_dialog_set_title (ADW_DIALOG (dialog), _("Preferences"));
  if (s == NULL)
    {
      AdwPreferencesPage *page = make_page (_("Preferences"), "emblem-system-symbolic");
      AdwPreferencesGroup *group = add_group (page, NULL, _("The settings schema is not installed, so preferences cannot be saved."));
      (void) group;
      adw_preferences_dialog_add (dialog, page);
    }
  else
    {
      build_appearance (dialog, s);
      build_new_picture (dialog, s);
      build_drawing (dialog, s);
      build_palette (dialog, s);
    }
  adw_dialog_present (ADW_DIALOG (dialog), parent);
}
