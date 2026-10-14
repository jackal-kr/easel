#include <adwaita.h>

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
  static const char *inspired_by[] = { "JS Paint by Isaiah Odhner https://jspaint.app", NULL };

  AdwDialog *dialog = adw_about_dialog_new ();
  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (dialog), "Easel");
  adw_about_dialog_set_developer_name (ADW_ABOUT_DIALOG (dialog), "Easel contributors");
  adw_about_dialog_set_version (ADW_ABOUT_DIALOG (dialog), EASEL_VERSION);
  adw_about_dialog_set_comments (ADW_ABOUT_DIALOG (dialog),
                                 "A simple paint program in the spirit of classic Microsoft Paint, "
                                 "built for GNOME.");
  adw_about_dialog_set_copyright (ADW_ABOUT_DIALOG (dialog), "© 2026 The Easel authors");
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (dialog), GTK_LICENSE_GPL_3_0);
  adw_about_dialog_add_acknowledgement_section (ADW_ABOUT_DIALOG (dialog), "Inspired by",
                                                inspired_by);
  adw_dialog_present (dialog, GTK_WIDGET (gtk_application_get_active_window (app)));
}

static const GActionEntry app_actions[] = {
  { "quit", action_quit },
  { "about", action_about },
};

static void
on_startup (GApplication *app)
{
  g_action_map_add_action_entries (G_ACTION_MAP (app), app_actions, G_N_ELEMENTS (app_actions), app);

  const char *quit_accels[] = { "<Primary>q", NULL };
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
  g_autoptr (AdwApplication) app = adw_application_new (EASEL_APP_ID, G_APPLICATION_HANDLES_OPEN);

  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "open", G_CALLBACK (on_open), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
