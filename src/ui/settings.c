#include "ui/settings.h"

#include <adwaita.h>
#include <stdint.h>
#include <string.h>

GSettings *
easel_settings (void)
{
  static GSettings *settings;
  static gboolean tried;

  if (tried)
    return settings;
  tried = TRUE;

  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  GSettingsSchema *schema = source != NULL ? g_settings_schema_source_lookup (source, EASEL_APP_ID, TRUE) : NULL;

  if (schema == NULL)
    {
      /* Running from the build directory: the compiled schema sits in ../data. */
      g_autofree char *exe = g_file_read_link ("/proc/self/exe", NULL);
      if (exe != NULL)
        {
          g_autofree char *dir = g_build_filename (g_path_get_dirname (exe), "..", "data", NULL);
          g_autofree char *compiled = g_build_filename (dir, "gschemas.compiled", NULL);
          if (g_file_test (compiled, G_FILE_TEST_EXISTS))
            {
              g_autoptr (GError) error = NULL;
              GSettingsSchemaSource *local = g_settings_schema_source_new_from_directory (dir, source, FALSE, &error);
              if (local != NULL)
                {
                  schema = g_settings_schema_source_lookup (local, EASEL_APP_ID, FALSE);
                  g_settings_schema_source_unref (local);
                }
            }
        }
    }
  if (schema != NULL)
    {
      settings = g_settings_new_full (schema, NULL, NULL);
      g_settings_schema_unref (schema);
    }
  else
    g_warning ("Easel settings schema not found; preferences will not be saved");
  return settings;
}

int
easel_pref_int (const char *key, int fallback)
{
  GSettings *s = easel_settings ();
  return s != NULL ? g_settings_get_int (s, key) : fallback;
}

gboolean
easel_pref_bool (const char *key, gboolean fallback)
{
  GSettings *s = easel_settings ();
  return s != NULL ? g_settings_get_boolean (s, key) : fallback;
}

char *
easel_pref_string (const char *key, const char *fallback)
{
  GSettings *s = easel_settings ();
  return s != NULL ? g_settings_get_string (s, key) : g_strdup (fallback);
}

gboolean
easel_parse_hex_color (const char *text, uint32_t *argb)
{
  if (text == NULL || text[0] != '#' || strlen (text) != 7)
    return FALSE;
  for (int i = 1; i < 7; i++)
    if (!g_ascii_isxdigit (text[i]))
      return FALSE;
  *argb = 0xFF000000u | (uint32_t) strtoul (text + 1, NULL, 16);
  return TRUE;
}

static GtkCssProvider *workspace_css;

static void
apply_appearance (void)
{
  g_autofree char *scheme = easel_pref_string ("color-scheme", "system");
  g_autofree char *color = easel_pref_string ("workspace-color", "");
  uint32_t argb;

  adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                      g_str_equal (scheme, "light") ? ADW_COLOR_SCHEME_FORCE_LIGHT
                                      : g_str_equal (scheme, "dark") ? ADW_COLOR_SCHEME_FORCE_DARK
                                      : ADW_COLOR_SCHEME_DEFAULT);

  if (workspace_css == NULL)
    {
      workspace_css = gtk_css_provider_new ();
      gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (workspace_css),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
  if (easel_parse_hex_color (color, &argb))
    {
      g_autofree char *css = g_strdup_printf (".easel-workspace { background-color: #%06x; }", argb & 0xFFFFFF);
      gtk_css_provider_load_from_string (workspace_css, css);
    }
  else
    gtk_css_provider_load_from_string (workspace_css, "");
}

static void
on_appearance_changed (GSettings *settings, const char *key, gpointer data)
{
  apply_appearance ();
}

void
easel_settings_apply_appearance (void)
{
  GSettings *s = easel_settings ();

  apply_appearance ();
  if (s != NULL)
    {
      g_signal_connect (s, "changed::color-scheme", G_CALLBACK (on_appearance_changed), NULL);
      g_signal_connect (s, "changed::workspace-color", G_CALLBACK (on_appearance_changed), NULL);
    }
}
