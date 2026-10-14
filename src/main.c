#include <adwaita.h>
#include <glib/gi18n.h>
#include <locale.h>

#include "ui/preferences.h"
#include "ui/settings.h"
#include "ui/window.h"

static void
action_quit (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  g_application_quit (G_APPLICATION (user_data));
}

static void
action_about (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  GtkApplication *app = user_data;
  const char *inspired_by[] = {
    _("Microsoft Paint in Windows XP, the original that inspired Easel"),
    "JS Paint by Isaiah Odhner https://jspaint.app",
    NULL
  };

  AdwDialog *dialog = adw_about_dialog_new ();
  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (dialog), "Easel");
  adw_about_dialog_set_developer_name (ADW_ABOUT_DIALOG (dialog), "JOD (Oleksiy Degtyar)");
  adw_about_dialog_set_version (ADW_ABOUT_DIALOG (dialog), EASEL_VERSION);
  adw_about_dialog_set_comments (ADW_ABOUT_DIALOG (dialog),
                                 _("A simple paint program in the spirit of classic Microsoft Paint, built for GNOME."));
  adw_about_dialog_set_copyright (ADW_ABOUT_DIALOG (dialog), "© 2026 JOD (Oleksiy Degtyar)");
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (dialog), GTK_LICENSE_GPL_3_0);
  adw_about_dialog_add_acknowledgement_section (ADW_ABOUT_DIALOG (dialog), _("Inspired by"),
                                                inspired_by);
  adw_dialog_present (dialog, GTK_WIDGET (gtk_application_get_active_window (app)));
}

static void
add_shortcuts (AdwShortcutsDialog *dialog, const char *title, const char *const *entries)
{
  AdwShortcutsSection *section = adw_shortcuts_section_new (title);

  for (int i = 0; entries[i] != NULL; i += 2)
    adw_shortcuts_section_add (section, adw_shortcuts_item_new (entries[i], entries[i + 1]));
  adw_shortcuts_dialog_add (dialog, section);
}

static void
action_shortcuts (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  GtkApplication *app = user_data;
  AdwShortcutsDialog *dialog = ADW_SHORTCUTS_DIALOG (adw_shortcuts_dialog_new ());

  const char *const file[] = {
    _("New"), "<Primary>n", _("Open…"), "<Primary>o", _("Save"), "<Primary>s", _("Save As…"), "<Primary><Shift>s",
    _("Print…"), "<Primary>p", _("Quit"), "<Primary>q", NULL };
  const char *const edit[] = {
    _("Undo"), "<Primary>z", _("Redo"), "<Primary>y", _("Redo (alternative)"), "<Primary><Shift>z",
    _("Cut"), "<Primary>x", _("Copy"), "<Primary>c", _("Paste"), "<Primary>v", _("Select All"), "<Primary>a",
    _("Clear Selection"), "Delete", NULL };
  const char *const view[] = {
    _("Normal Size"), "<Primary>Page_Down", _("Large Size"), "<Primary>Page_Up", _("Zoom with the wheel"), "<Primary>Scroll_Up",
    _("Show Grid"), "<Primary>g", NULL };
  const char *const image[] = {
    _("Flip/Rotate…"), "<Primary>r", _("Invert Colors"), "<Primary>i", _("Attributes…"), "<Primary>e",
    _("Clear Image"), "<Primary><Shift>n", _("Swap Foreground/Background"), "<Primary><Shift>x", NULL };
  const char *const general[] = {
    _("Keyboard Shortcuts"), "<Primary>question", _("Cancel the current drawing or selection"), "Escape", NULL };

  add_shortcuts (dialog, _("File"), file);
  add_shortcuts (dialog, _("Edit"), edit);
  add_shortcuts (dialog, _("View"), view);
  add_shortcuts (dialog, _("Image and Colors"), image);
  add_shortcuts (dialog, _("General"), general);
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (gtk_application_get_active_window (app)));
}

static void
action_preferences (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_preferences_present (GTK_WIDGET (gtk_application_get_active_window (user_data)));
}

/* Help opens docs/help.md in the user's default Markdown/text viewer. It is
 * found next to the source tree when running from the build directory, else
 * in the installed doc directory. */
static void
action_help (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  GtkApplication *app = user_data;
  g_autofree char *exe = g_file_read_link ("/proc/self/exe", NULL);
  g_autofree char *exe_dir = exe != NULL ? g_path_get_dirname (exe) : NULL;
  g_autofree char *dev = exe_dir != NULL ? g_build_filename (exe_dir, "..", "..", "docs", "help.md", NULL) : NULL;
  g_autofree char *installed = g_build_filename (EASEL_DOCDIR, "help.md", NULL);
  const char *path = dev != NULL && g_file_test (dev, G_FILE_TEST_EXISTS) ? dev : installed;
  GtkWindow *parent = gtk_application_get_active_window (app);

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    {
      AdwDialog *dialog = adw_alert_dialog_new (_("Help Not Found"), _("The help file is not installed."));

      adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "ok", "_OK");
      adw_dialog_present (dialog, GTK_WIDGET (parent));
      return;
    }

  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autoptr (GtkFileLauncher) launcher = gtk_file_launcher_new (file);

  gtk_file_launcher_launch (launcher, parent, NULL, NULL, NULL);
}

static const GActionEntry app_actions[] = {
  { "help", action_help },
  { "preferences", action_preferences },
  { "shortcuts", action_shortcuts },
  { "quit", action_quit },
  { "about", action_about },
};

static void
on_startup (GApplication *app)
{
  g_action_map_add_action_entries (G_ACTION_MAP (app), app_actions, G_N_ELEMENTS (app_actions), app);

  const char *quit_accels[] = { "<Primary>q", NULL };
  const char *prefs_accels[] = { "<Primary>comma", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "app.preferences", prefs_accels);
  const char *shortcuts_accels[] = { "<Primary>question", "<Primary>F1", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "app.shortcuts", shortcuts_accels);
  const char *help_accels[] = { "F1", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "app.help", help_accels);
  const char *swap_accels[] = { "<Primary><Shift>x", NULL };
  const char *clear_accels[] = { "<Primary><Shift>n", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "app.quit", quit_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.swap-colors", swap_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.clear-image", clear_accels);
  const char *new_accels[] = { "<Primary>n", NULL };
  const char *open_accels[] = { "<Primary>o", NULL };
  const char *save_accels[] = { "<Primary>s", NULL };
  const char *save_as_accels[] = { "<Primary><Shift>s", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.new", new_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.open", open_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.save", save_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.save-as", save_as_accels);
  const char *attr_accels[] = { "<Primary>e", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.attributes", attr_accels);
  const char *flip_accels[] = { "<Primary>r", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.flip-rotate", flip_accels);
  const char *invert_accels[] = { "<Primary>i", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.invert-colors", invert_accels);
  const char *print_accels[] = { "<Primary>p", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.print", print_accels);
  const char *grid_accels[] = { "<Primary>g", NULL };
  const char *large_accels[] = { "<Primary>Page_Up", NULL };
  const char *normal_accels[] = { "<Primary>Page_Down", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.show-grid", grid_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.zoom-large", large_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.zoom-normal", normal_accels);
  const char *cut_accels[] = { "<Primary>x", NULL };
  const char *copy_accels[] = { "<Primary>c", NULL };
  const char *paste_accels[] = { "<Primary>v", NULL };
  const char *all_accels[] = { "<Primary>a", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.cut", cut_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.copy", copy_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.paste", paste_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.select-all", all_accels);
  const char *undo_accels[] = { "<Primary>z", NULL };
  const char *redo_accels[] = { "<Primary>y", "<Primary><Shift>z", NULL };
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.undo", undo_accels);
  gtk_application_set_accels_for_action (GTK_APPLICATION (app), "win.redo", redo_accels);

  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (css, "/io/github/jackal_kr/Easel/style.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                              GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (css);

  /* Application icon: installed in the icon theme, or from the source tree
   * when running out of the build directory. */
  g_autofree char *exe = g_file_read_link ("/proc/self/exe", NULL);
  g_autofree char *exe_dir = exe != NULL ? g_path_get_dirname (exe) : NULL;
  g_autofree char *dev_icons = exe_dir != NULL ? g_build_filename (exe_dir, "..", "..", "data", "icons", NULL) : NULL;

  if (dev_icons != NULL && g_file_test (dev_icons, G_FILE_TEST_IS_DIR))
    gtk_icon_theme_add_search_path (gtk_icon_theme_get_for_display (gdk_display_get_default ()), dev_icons);
  gtk_window_set_default_icon_name (EASEL_APP_ID);

  easel_settings_apply_appearance ();
}

static void
on_activate (GApplication *app)
{
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));

  if (window == NULL)
    window = GTK_WINDOW (easel_window_new (GTK_APPLICATION (app)));
  gtk_window_present (window);
}

static void
on_open (GApplication *app, GFile **files, int n_files, const char *hint)
{
  EaselWindow *window = EASEL_WINDOW (easel_window_new (GTK_APPLICATION (app)));

  gtk_window_present (GTK_WINDOW (window));
  if (n_files > 0)
    easel_window_open_file (window, files[0]);
}

int
main (int argc, char *argv[])
{
  /* EASEL_LOCALEDIR lets a build tree or test install use its own catalogs. */
  const char *localedir = g_getenv ("EASEL_LOCALEDIR");

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, localedir != NULL ? localedir : LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  g_autoptr (AdwApplication) app = adw_application_new (EASEL_APP_ID, G_APPLICATION_HANDLES_OPEN);

  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "open", G_CALLBACK (on_open), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
