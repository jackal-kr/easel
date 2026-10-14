#include "ui/window.h"

#include <math.h>

#include "core/document.h"
#include "ui/canvas.h"
#include "ui/colorbox.h"
#include "ui/fileio.h"
#include "ui/settings.h"
#include "ui/toolbox.h"

/* Size and background of a new picture come from Preferences. */
#define DEFAULT_IMAGE_WIDTH  easel_pref_int ("default-width", 800)
#define DEFAULT_IMAGE_HEIGHT easel_pref_int ("default-height", 600)

static uint32_t
default_background (void)
{
  g_autofree char *hex = easel_pref_string ("default-background", "#ffffff");
  uint32_t argb;

  return easel_parse_hex_color (hex, &argb) ? argb : EASEL_RGB (255, 255, 255);
}

struct _EaselWindow
{
  AdwApplicationWindow parent_instance;

  EaselCanvas   *canvas;
  EaselToolbox  *toolbox;
  EaselColorBox *colorbox;

  GtkWidget *status_bar;
  GtkLabel  *hint_label;
  GtkLabel  *position_label;
  GtkLabel  *size_label;
  GtkLabel  *zoom_label;
  GtkLabel  *menu_zoom_label; /* zoom row in the main menu */
  GtkLabel  *frame_label;   /* View > Frame Time */
  guint      frame_timer;

  GtkWidget *thumb_window;  /* View > Zoom > Show Thumbnail */
  GtkWidget *thumb_area;
  guint      thumb_timer;

  GFile    *file;     /* where the picture was last opened from / saved to */
  gboolean  modified;
  gboolean  closing;  /* close confirmed: let the window go */
};

G_DEFINE_FINAL_TYPE (EaselWindow, easel_window, ADW_TYPE_APPLICATION_WINDOW)

static void
on_pointer_moved (EaselCanvas *canvas, int x, int y, EaselWindow *self)
{
  if (x < 0 || y < 0)
    {
      gtk_label_set_text (self->position_label, "");
      return;
    }

  g_autofree char *text = g_strdup_printf ("%d, %d", x, y);
  gtk_label_set_text (self->position_label, text);
}

static void
on_colors_changed (EaselColorBox *box, EaselWindow *self)
{
  easel_canvas_set_foreground (self->canvas, easel_color_box_get_foreground (box));
  easel_canvas_set_background (self->canvas, easel_color_box_get_background (box));
}

/* Pushes the toolbox's current tool and options to the canvas. */
static void
sync_canvas_tool (EaselWindow *self)
{
  easel_canvas_set_tool (self->canvas, easel_toolbox_get_tool (self->toolbox));
  easel_canvas_set_tool_size (self->canvas, easel_toolbox_get_tool_size (self->toolbox));
  easel_canvas_set_brush_shape (self->canvas, easel_toolbox_get_brush_shape (self->toolbox));
  easel_canvas_set_line_width (self->canvas, easel_toolbox_get_line_width (self->toolbox));
  easel_canvas_set_fill_style (self->canvas, easel_toolbox_get_fill_style (self->toolbox));
  easel_canvas_set_zoom_level (self->canvas, easel_toolbox_get_zoom_level (self->toolbox));
  easel_canvas_set_opaque (self->canvas, easel_toolbox_get_opaque (self->toolbox));
  easel_canvas_set_font (self->canvas, easel_toolbox_get_font (self->toolbox));
  easel_canvas_set_text_opaque (self->canvas, easel_toolbox_get_text_opaque (self->toolbox));
  easel_canvas_set_text_smooth (self->canvas, easel_toolbox_get_text_smooth (self->toolbox));
  easel_canvas_set_gradient_style (self->canvas, easel_toolbox_get_gradient_style (self->toolbox));
}

static void
on_tool_changed (EaselToolbox *toolbox, EaselWindow *self)
{
  gtk_label_set_text (self->hint_label, easel_toolbox_get_tool_hint (toolbox));
  sync_canvas_tool (self);
}

static void
on_options_changed (EaselToolbox *toolbox, EaselWindow *self)
{
  sync_canvas_tool (self);
  /* Choosing a magnification while the Magnifier is active applies it now. */
  if (easel_toolbox_get_tool (toolbox) == EASEL_TOOL_MAGNIFIER)
    easel_canvas_set_zoom (self->canvas, easel_toolbox_get_zoom_level (toolbox) * 100, -1, -1);
}

/* Pick Color sets the chosen colour and then hands control back to the tool
 * that was active before, as classic Paint does. */
static void
on_color_picked (EaselCanvas *canvas, guint argb, gboolean foreground, EaselWindow *self)
{
  if (foreground)
    easel_color_box_set_foreground (self->colorbox, argb);
  else
    easel_color_box_set_background (self->colorbox, argb);
  easel_toolbox_select_previous_tool (self->toolbox);
}

static void
action_swap_colors (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselWindow *self = user_data;
  easel_color_box_swap (self->colorbox);
}

/* Classic Paint fills the whole image with the background colour. */
static void
action_clear_image (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselWindow *self = user_data;
  easel_canvas_clear (self->canvas, easel_color_box_get_background (self->colorbox));
}

static void
action_undo (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_canvas_undo (((EaselWindow *) user_data)->canvas);
}

static void
action_redo (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_canvas_redo (((EaselWindow *) user_data)->canvas);
}

static void
update_title (EaselWindow *self)
{
  g_autofree char *name = self->file != NULL ? g_file_get_basename (self->file) : g_strdup ("Untitled");
  g_autofree char *title = g_strdup_printf ("%s%s - Easel", self->modified ? "*" : "", name);

  gtk_window_set_title (GTK_WINDOW (self), title);
}

static void
set_modified (EaselWindow *self, gboolean modified)
{
  self->modified = modified;
  update_title (self);
}

static void
update_size_label (EaselWindow *self)
{
  EaselDocument *doc = easel_canvas_get_document (self->canvas);
  g_autofree char *text = g_strdup_printf ("%d × %d px", doc->width, doc->height);

  gtk_label_set_text (self->size_label, text);
}

static void
on_history_changed (EaselCanvas *canvas, EaselWindow *self)
{
  set_modified (self, !easel_canvas_is_saved (canvas));
  update_size_label (self);
  g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self), "undo")),
                               easel_canvas_can_undo (canvas));
  g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self), "redo")),
                               easel_canvas_can_redo (canvas));
}


/* ---- Files ---------------------------------------------------------- */

typedef void (*ContinueFunc) (EaselWindow *self);

typedef struct
{
  EaselWindow *self;
  ContinueFunc next;
} Pending;

static Pending *
pending_new (EaselWindow *self, ContinueFunc next)
{
  Pending *p = g_new0 (Pending, 1);
  p->self = g_object_ref (self);
  p->next = next;
  return p;
}

static void
pending_free (Pending *p)
{
  g_object_unref (p->self);
  g_free (p);
}

static void
show_error (EaselWindow *self, const char *heading, const GError *error)
{
  AdwDialog *dialog = adw_alert_dialog_new (heading, error != NULL ? error->message : NULL);

  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "ok", "_OK");
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static GtkFileDialog *
make_file_dialog (void)
{
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  GtkFileFilter *pictures = gtk_file_filter_new ();

  gtk_file_filter_set_name (pictures, "Pictures");
  gtk_file_filter_add_mime_type (pictures, "image/png");
  gtk_file_filter_add_mime_type (pictures, "image/jpeg");
  gtk_file_filter_add_mime_type (pictures, "image/bmp");
  gtk_file_filter_add_mime_type (pictures, "image/tiff");
  g_list_store_append (filters, pictures);
  g_object_unref (pictures);

  static const struct { const char *name, *mime; } each[] = {
    { "PNG", "image/png" }, { "JPEG", "image/jpeg" }, { "BMP", "image/bmp" }, { "TIFF", "image/tiff" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (each); i++)
    {
      GtkFileFilter *f = gtk_file_filter_new ();
      gtk_file_filter_set_name (f, each[i].name);
      gtk_file_filter_add_mime_type (f, each[i].mime);
      g_list_store_append (filters, f);
      g_object_unref (f);
    }
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  g_object_unref (filters);
  return dialog;
}

/* Writes the picture and, on success, adopts `file` as its home. */
static gboolean
save_to (EaselWindow *self, GFile *file)
{
  g_autoptr (GError) error = NULL;

  easel_canvas_flush (self->canvas); /* a floating selection is part of the picture */
  if (!easel_file_save (easel_canvas_get_document (self->canvas), file, &error))
    {
      show_error (self, "Could not save the picture", error);
      return FALSE;
    }
  g_set_object (&self->file, file);
  easel_canvas_mark_saved (self->canvas);
  set_modified (self, FALSE);
  return TRUE;
}

static void
on_save_as_done (GObject *source, GAsyncResult *result, gpointer data)
{
  Pending *p = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      g_autofree char *name = g_file_get_basename (file);
      g_autoptr (GFile) target = g_object_ref (file);

      if (!easel_file_has_known_extension (name))
        {
          g_autofree char *with_ext = g_strconcat (name, ".png", NULL);
          g_autoptr (GFile) parent = g_file_get_parent (file);
          g_clear_object (&target);
          target = g_file_get_child (parent, with_ext);
        }
      if (save_to (p->self, target) && p->next != NULL)
        p->next (p->self);
    }
  pending_free (p);
}

static void
save_as (EaselWindow *self, ContinueFunc next)
{
  GtkFileDialog *dialog = make_file_dialog ();

  gtk_file_dialog_set_title (dialog, "Save As");
  if (self->file != NULL)
    gtk_file_dialog_set_initial_file (dialog, self->file);
  else
    gtk_file_dialog_set_initial_name (dialog, "Untitled.png");
  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL, on_save_as_done, pending_new (self, next));
  g_object_unref (dialog);
}

static void
save (EaselWindow *self, ContinueFunc next)
{
  if (self->file == NULL)
    save_as (self, next);
  else if (save_to (self, self->file) && next != NULL)
    next (self);
}

static void
on_unsaved_answer (GObject *source, GAsyncResult *result, gpointer data)
{
  Pending *p = data;
  const char *answer = adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result);

  if (g_str_equal (answer, "save"))
    save (p->self, p->next);
  else if (g_str_equal (answer, "discard"))
    p->next (p->self);
  pending_free (p);
}

/* Runs `next` once it is safe to throw the current picture away. */
static void
confirm_discard (EaselWindow *self, ContinueFunc next)
{
  if (!self->modified)
    {
      next (self);
      return;
    }

  g_autofree char *name = self->file != NULL ? g_file_get_basename (self->file) : g_strdup ("Untitled");
  g_autofree char *body = g_strdup_printf ("Save changes to “%s” before closing?", name);
  AdwDialog *dialog = adw_alert_dialog_new ("Save Changes?", body);

  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                  "cancel", "_Cancel", "discard", "_Discard", "save", "_Save", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "discard",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "save",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "save");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  adw_alert_dialog_choose (ADW_ALERT_DIALOG (dialog), GTK_WIDGET (self), NULL,
                           on_unsaved_answer, pending_new (self, next));
}

static void
show_document (EaselWindow *self, EaselDocument *doc, GFile *file)
{
  easel_canvas_set_document (self->canvas, doc);
  easel_toolbox_select_tool (self->toolbox, EASEL_TOOL_PAN); /* a fresh picture starts in Pan */
  g_set_object (&self->file, file);
  update_size_label (self);
  set_modified (self, FALSE);
}

static void
do_new (EaselWindow *self)
{
  EaselDocument *doc = easel_document_new (DEFAULT_IMAGE_WIDTH, DEFAULT_IMAGE_HEIGHT, default_background ());
  if (doc != NULL)
    show_document (self, doc, NULL);
}

static void
on_open_done (GObject *source, GAsyncResult *result, gpointer data)
{
  EaselWindow *self = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    easel_window_open_file (self, file);
  g_object_unref (self);
}

static void
do_open (EaselWindow *self)
{
  GtkFileDialog *dialog = make_file_dialog ();

  gtk_file_dialog_set_title (dialog, "Open");
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL, on_open_done, g_object_ref (self));
  g_object_unref (dialog);
}

/* Opens `file` without asking about unsaved work (callers have already asked). */
static void
load_file (EaselWindow *self, GFile *file)
{
  g_autoptr (GError) error = NULL;
  EaselDocument *doc = easel_file_load (file, &error);

  if (doc == NULL)
    show_error (self, "Could not open the picture", error);
  else
    show_document (self, doc, file);
}

static GFile *pending_open_file; /* handed from easel_window_open_file to its continuation */

static void
open_pending (EaselWindow *self)
{
  g_autoptr (GFile) file = g_steal_pointer (&pending_open_file);
  if (file != NULL)
    load_file (self, file);
}

void
easel_window_open_file (EaselWindow *self, GFile *file)
{
  g_return_if_fail (EASEL_IS_WINDOW (self));
  g_set_object (&pending_open_file, file);
  confirm_discard (self, open_pending);
}

static void
action_new (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  confirm_discard (user_data, do_new);
}

static void
action_open (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  do_open (user_data); /* unsaved work is asked about once a file is picked */
}

static void
action_save (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  save (user_data, NULL);
}

static void
action_save_as (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  save_as (user_data, NULL);
}

static void
close_confirmed (EaselWindow *self)
{
  self->closing = TRUE;
  gtk_window_close (GTK_WINDOW (self));
}

static gboolean
on_close_request (GtkWindow *window, gpointer user_data)
{
  EaselWindow *self = EASEL_WINDOW (window);

  if (self->closing || !self->modified)
    return FALSE;
  confirm_discard (self, close_confirmed);
  return TRUE; /* hold the window until the user has answered */
}


/* ---- Image > Attributes --------------------------------------------- */

#define SCREEN_DPI 96.0

typedef struct
{
  EaselWindow *self;
  AdwSpinRow  *width_row, *height_row;
  AdwComboRow *units_row, *colors_row;
  int          unit;        /* 0 pixels, 1 inches, 2 centimetres */
  double       px_w, px_h;  /* the size in pixels, whatever the unit shows */
} AttrState;

static double
px_per_unit (int unit)
{
  return unit == 1 ? SCREEN_DPI : (unit == 2 ? SCREEN_DPI / 2.54 : 1.0);
}

static void
attr_show_size (AttrState *st)
{
  double k = px_per_unit (st->unit);
  GtkAdjustment *aw = adw_spin_row_get_adjustment (st->width_row);
  GtkAdjustment *ah = adw_spin_row_get_adjustment (st->height_row);
  int digits = st->unit == 0 ? 0 : 2;

  adw_spin_row_set_digits (st->width_row, digits);
  adw_spin_row_set_digits (st->height_row, digits);
  gtk_adjustment_configure (aw, st->px_w / k, 1 / k, 16384 / k, st->unit == 0 ? 1 : 0.1, 10, 0);
  gtk_adjustment_configure (ah, st->px_h / k, 1 / k, 16384 / k, st->unit == 0 ? 1 : 0.1, 10, 0);
}

static void
attr_read_size (AttrState *st)
{
  double k = px_per_unit (st->unit);

  st->px_w = CLAMP (gtk_adjustment_get_value (adw_spin_row_get_adjustment (st->width_row)) * k, 1, 16384);
  st->px_h = CLAMP (gtk_adjustment_get_value (adw_spin_row_get_adjustment (st->height_row)) * k, 1, 16384);
}

static void
on_attr_units_changed (AdwComboRow *row, GParamSpec *pspec, AttrState *st)
{
  attr_read_size (st); /* in the old unit */
  st->unit = (int) adw_combo_row_get_selected (row);
  attr_show_size (st);
}

static void
on_attr_response (AdwAlertDialog *dialog, const char *response, AttrState *st)
{
  if (g_str_equal (response, "ok"))
    {
      attr_read_size (st);
      easel_canvas_resize (st->self->canvas, (int) lround (st->px_w), (int) lround (st->px_h));
      if (adw_combo_row_get_selected (st->colors_row) == 1)
        easel_canvas_black_and_white (st->self->canvas);
    }
}

static void
on_attr_default (GtkButton *button, AttrState *st)
{
  st->px_w = DEFAULT_IMAGE_WIDTH;
  st->px_h = DEFAULT_IMAGE_HEIGHT;
  attr_show_size (st);
}

static void
action_attributes (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselWindow *self = user_data;
  EaselDocument *doc = easel_canvas_get_document (self->canvas);
  AttrState *st = g_new0 (AttrState, 1);
  AdwDialog *dialog = adw_alert_dialog_new ("Attributes", NULL);

  st->self = self;
  st->px_w = doc->width;
  st->px_h = doc->height;

  GtkWidget *group = adw_preferences_group_new ();
  st->width_row = ADW_SPIN_ROW (adw_spin_row_new_with_range (1, 16384, 1));
  st->height_row = ADW_SPIN_ROW (adw_spin_row_new_with_range (1, 16384, 1));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (st->width_row), "Width");
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (st->height_row), "Height");

  st->units_row = ADW_COMBO_ROW (adw_combo_row_new ());
  const char *const units[] = { "Pixels", "Inches", "Centimeters", NULL };
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (st->units_row), "Units");
  adw_combo_row_set_model (st->units_row, G_LIST_MODEL (gtk_string_list_new (units)));

  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (st->width_row));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (st->height_row));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (st->units_row));

  st->colors_row = ADW_COMBO_ROW (adw_combo_row_new ());
  const char *const colors[] = { "Colors", "Black and white", NULL };
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (st->colors_row), "Color mode");
  adw_combo_row_set_model (st->colors_row, G_LIST_MODEL (gtk_string_list_new (colors)));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (st->colors_row));
  attr_show_size (st);
  g_signal_connect (st->units_row, "notify::selected", G_CALLBACK (on_attr_units_changed), st);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *reset = gtk_button_new_with_mnemonic ("_Default Size");
  gtk_widget_set_halign (reset, GTK_ALIGN_END);
  g_signal_connect (reset, "clicked", G_CALLBACK (on_attr_default), st);
  gtk_box_append (GTK_BOX (box), group);
  gtk_box_append (GTK_BOX (box), reset);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), box);
  adw_alert_dialog_set_body (ADW_ALERT_DIALOG (dialog),
                             "New area is filled with the background color; a smaller size crops the picture. Black and white converts the picture and cannot be reverted except with Undo.");
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                  "cancel", "_Cancel", "ok", "_OK", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "ok", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "ok");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_signal_connect (dialog, "response", G_CALLBACK (on_attr_response), st);
  g_object_set_data_full (G_OBJECT (dialog), "attr-state", st, g_free);
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
on_resize_preview (EaselCanvas *canvas, int w, int h, EaselWindow *self)
{
  if (w < 0)
    {
      update_size_label (self);
      return;
    }
  g_autofree char *text = g_strdup_printf ("%d × %d px", w, h);
  gtk_label_set_text (self->size_label, text);
}


/* ---- View > Zoom, Grid ---------------------------------------------- */

static void set_action_enabled (EaselWindow *self, const char *name, gboolean enabled);

static void
on_zoom_changed (EaselCanvas *canvas, EaselWindow *self)
{
  g_autofree char *text = g_strdup_printf ("%d%%", easel_canvas_get_zoom (canvas));
  gtk_label_set_text (self->zoom_label, text);
  if (self->menu_zoom_label != NULL)
    gtk_label_set_text (self->menu_zoom_label, text);
  /* As in XP, the pixel grid only exists from 4x up, so the item is off below that. */
  set_action_enabled (self, "show-grid", easel_canvas_get_zoom (canvas) >= 400);
}

static void
action_zoom_normal (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_canvas_set_zoom (((EaselWindow *) user_data)->canvas, 100, -1, -1);
}

static void
action_zoom_in (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_canvas_zoom_step (((EaselWindow *) user_data)->canvas, 1, -1, -1);
}

static void
action_zoom_out (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_canvas_zoom_step (((EaselWindow *) user_data)->canvas, -1, -1, -1);
}

static void
action_zoom_large (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_canvas_set_zoom (((EaselWindow *) user_data)->canvas, 400, -1, -1);
}

static const int zoom_choices[] = { 1, 2, 4, 6, 8 };

static void
on_zoom_custom_response (AdwAlertDialog *dialog, const char *response, EaselWindow *self)
{
  if (!g_str_equal (response, "ok"))
    return;

  AdwComboRow *row = g_object_get_data (G_OBJECT (dialog), "zoom-row");
  guint i = adw_combo_row_get_selected (row);
  if (i < G_N_ELEMENTS (zoom_choices))
    easel_canvas_set_zoom (self->canvas, zoom_choices[i] * 100, -1, -1);
}

static void
action_zoom_custom (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselWindow *self = user_data;
  AdwDialog *dialog = adw_alert_dialog_new ("Custom Zoom", NULL);
  AdwComboRow *row = ADW_COMBO_ROW (adw_combo_row_new ());
  const char *const labels[] = { "100%", "200%", "400%", "600%", "800%", NULL };
  GtkWidget *group = adw_preferences_group_new ();
  guint current = 0;

  for (guint i = 0; i < G_N_ELEMENTS (zoom_choices); i++)
    if (zoom_choices[i] * 100 == easel_canvas_get_zoom (self->canvas))
      current = i;
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), "Zoom to");
  adw_combo_row_set_model (row, G_LIST_MODEL (gtk_string_list_new (labels)));
  adw_combo_row_set_selected (row, current);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (row));

  adw_dialog_set_content_width (dialog, 440);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), group);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog), "cancel", "_Cancel", "ok", "_OK", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "ok", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "ok");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_object_set_data (G_OBJECT (dialog), "zoom-row", row);
  g_signal_connect (dialog, "response", G_CALLBACK (on_zoom_custom_response), self);
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
on_grid_change_state (GSimpleAction *action, GVariant *value, gpointer user_data)
{
  EaselWindow *self = user_data;

  g_simple_action_set_state (action, value);
  easel_canvas_set_show_grid (self->canvas, g_variant_get_boolean (value));
}


/* ---- Edit: selection and clipboard ---------------------------------- */

static void
set_action_enabled (EaselWindow *self, const char *name, gboolean enabled)
{
  GAction *action = g_action_map_lookup_action (G_ACTION_MAP (self), name);
  if (action != NULL)
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), enabled);
}

static void
on_selection_changed (EaselCanvas *canvas, EaselWindow *self)
{
  gboolean has = easel_canvas_has_selection (canvas);

  set_action_enabled (self, "cut", has);
  set_action_enabled (self, "copy", has);
  set_action_enabled (self, "clear-selection", has);
  set_action_enabled (self, "copy-to", has);
}

static void
on_copy_to_done (GObject *source, GAsyncResult *result, gpointer data)
{
  EaselWindow *self = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);
  EaselClip *clip = file != NULL ? easel_canvas_get_selection (self->canvas) : NULL;

  if (clip != NULL)
    {
      EaselDocument *doc = easel_document_new (clip->width, clip->height, 0);
      g_autofree char *name = g_file_get_basename (file);
      g_autoptr (GFile) target = g_object_ref (file);

      if (doc != NULL)
        {
          for (gsize i = 0; i < (gsize) clip->width * clip->height; i++)
            doc->pixels[i] = (clip->mask != NULL && !clip->mask[i]) ? 0 : clip->pixels[i];
          if (!easel_file_has_known_extension (name))
            {
              g_autofree char *with_ext = g_strconcat (name, ".png", NULL);
              g_autoptr (GFile) parent = g_file_get_parent (file);
              g_clear_object (&target);
              target = g_file_get_child (parent, with_ext);
            }
          if (!easel_file_save (doc, target, &error))
            show_error (self, "Could not save the selection", error);
          easel_document_free (doc);
        }
      easel_clip_free (clip);
    }
  g_object_unref (self);
}

static void
action_copy_to (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselWindow *self = user_data;
  GtkFileDialog *dialog = make_file_dialog ();

  gtk_file_dialog_set_title (dialog, "Copy To");
  gtk_file_dialog_set_initial_name (dialog, "Selection.png");
  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL, on_copy_to_done, g_object_ref (self));
  g_object_unref (dialog);
}

static void
on_paste_from_done (GObject *source, GAsyncResult *result, gpointer data)
{
  EaselWindow *self = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      EaselDocument *doc = easel_file_load (file, &error);

      if (doc == NULL)
        show_error (self, "Could not open the picture", error);
      else
        {
          EaselClip *clip = easel_clip_new (doc->width, doc->height);

          if (clip != NULL)
            {
              memcpy (clip->pixels, doc->pixels, (gsize) doc->width * doc->height * 4);
              easel_toolbox_select_tool (self->toolbox, EASEL_TOOL_SELECT);
              easel_canvas_paste_clip (self->canvas, clip);
            }
          easel_document_free (doc);
        }
    }
  g_object_unref (self);
}

static void
action_paste_from (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselWindow *self = user_data;
  GtkFileDialog *dialog = make_file_dialog ();

  gtk_file_dialog_set_title (dialog, "Paste From");
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL, on_paste_from_done, g_object_ref (self));
  g_object_unref (dialog);
}

static void
on_text_style_toggled (EaselToolbox *toolbox, guint flag, gboolean on, EaselWindow *self)
{
  easel_canvas_text_apply_style (self->canvas, flag, on);
}

static void
on_text_state (EaselCanvas *canvas, guint flags, EaselWindow *self)
{
  easel_toolbox_set_text_style (self->toolbox, flags);
}

static void
action_cut (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  if (easel_canvas_text_active (((EaselWindow *) user_data)->canvas))
    {
      easel_canvas_text_cut (((EaselWindow *) user_data)->canvas);
      return;
    }
  easel_canvas_cut (((EaselWindow *) user_data)->canvas);
}

static void
action_copy (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  if (easel_canvas_text_active (((EaselWindow *) user_data)->canvas))
    {
      easel_canvas_text_copy (((EaselWindow *) user_data)->canvas);
      return;
    }
  easel_canvas_copy (((EaselWindow *) user_data)->canvas);
}

static void
action_clear_selection (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  if (easel_canvas_text_active (((EaselWindow *) user_data)->canvas))
    {
      easel_canvas_text_delete (((EaselWindow *) user_data)->canvas);
      return;
    }
  easel_canvas_clear_selection (((EaselWindow *) user_data)->canvas);
}

static void
action_paste (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselWindow *self = user_data;

  if (easel_canvas_text_active (self->canvas))
    {
      easel_canvas_text_paste (self->canvas);
      return;
    }
  easel_toolbox_select_tool (self->toolbox, EASEL_TOOL_SELECT);
  easel_canvas_paste (self->canvas);
}

static void
action_select_all (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselWindow *self = user_data;

  if (easel_canvas_text_active (self->canvas))
    {
      easel_canvas_text_select_all (self->canvas);
      return;
    }
  easel_toolbox_select_tool (self->toolbox, EASEL_TOOL_SELECT);
  easel_canvas_select_all (self->canvas);
}


/* ---- Image: flip/rotate, stretch/skew, invert ------------------------- */

static void
action_edit_colors (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_color_box_edit_colors (((EaselWindow *) user_data)->colorbox);
}

static void
action_invert_colors (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_canvas_invert (((EaselWindow *) user_data)->canvas);
}

typedef struct
{
  EaselWindow *window;
  GtkWidget   *choices[5];
} FlipDialog;

static void
on_flip_response (AdwAlertDialog *dialog, const char *response, FlipDialog *fd)
{
  if (g_str_equal (response, "ok"))
    for (int i = 0; i < 5; i++)
      if (gtk_check_button_get_active (GTK_CHECK_BUTTON (fd->choices[i])))
        easel_canvas_turn (fd->window->canvas, (EaselTurn) i);
  g_free (fd);
}

/* ---- File > Print ---------------------------------------------------- */

static GtkPrintSettings *print_settings;
static GtkPageSetup *page_setup;

static void
on_draw_page (GtkPrintOperation *op, GtkPrintContext *context, int page_nr, EaselWindow *self)
{
  EaselDocument *doc = easel_canvas_get_document (self->canvas);
  cairo_t *cr = gtk_print_context_get_cairo_context (context);
  double pw = gtk_print_context_get_width (context), ph = gtk_print_context_get_height (context);
  double dpi = gtk_print_context_get_dpi_x (context);
  /* Actual size (96 pixels to the inch), shrunk to fit the page. */
  double scale = dpi / SCREEN_DPI;

  scale = MIN (scale, MIN (pw / doc->width, ph / doc->height));

  cairo_surface_t *surface = cairo_image_surface_create_for_data ((unsigned char *) doc->pixels,
                                                                  CAIRO_FORMAT_ARGB32, doc->width,
                                                                  doc->height, doc->width * 4);
  cairo_scale (cr, scale, scale);
  cairo_set_source_surface (cr, surface, 0, 0);
  cairo_pattern_set_filter (cairo_get_source (cr), scale >= 1 ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_BEST);
  cairo_paint (cr);
  cairo_surface_destroy (surface);
}

static void
on_print_done (GtkPrintOperation *op, GtkPrintOperationResult result, EaselWindow *self)
{
  if (result == GTK_PRINT_OPERATION_RESULT_APPLY)
    {
      g_clear_object (&print_settings);
      print_settings = g_object_ref (gtk_print_operation_get_print_settings (op));
    }
  g_object_unref (op);
}

static void
action_print (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselWindow *self = user_data;
  GtkPrintOperation *op = gtk_print_operation_new ();
  g_autoptr (GError) error = NULL;

  easel_canvas_flush (self->canvas);
  gtk_print_operation_set_n_pages (op, 1);
  gtk_print_operation_set_unit (op, GTK_UNIT_POINTS);
  if (print_settings != NULL)
    gtk_print_operation_set_print_settings (op, print_settings);
  if (page_setup != NULL)
    gtk_print_operation_set_default_page_setup (op, page_setup);
  g_signal_connect (op, "draw-page", G_CALLBACK (on_draw_page), self);
  g_signal_connect (op, "done", G_CALLBACK (on_print_done), self);
  gtk_print_operation_run (op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, GTK_WINDOW (self), &error);
  if (error != NULL)
    {
      show_error (self, "Could not print the picture", error); /* "done" releases op */
    }
}

static void
action_flip_rotate (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselWindow *self = user_data;
  static const char *const labels[5] = {
    "Flip horizontal", "Flip vertical", "Rotate 90° clockwise", "Rotate 180°", "Rotate 270° clockwise",
  };
  FlipDialog *fd = g_new0 (FlipDialog, 1);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  AdwDialog *dialog = adw_alert_dialog_new ("Flip and Rotate", NULL);

  fd->window = self;
  for (int i = 0; i < 5; i++)
    {
      fd->choices[i] = gtk_check_button_new_with_label (labels[i]);
      if (i > 0)
        gtk_check_button_set_group (GTK_CHECK_BUTTON (fd->choices[i]), GTK_CHECK_BUTTON (fd->choices[0]));
      gtk_box_append (GTK_BOX (box), fd->choices[i]);
    }
  gtk_check_button_set_active (GTK_CHECK_BUTTON (fd->choices[0]), TRUE);

  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), box);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog), "cancel", "_Cancel", "ok", "_OK", NULL);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "ok");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_signal_connect (dialog, "response", G_CALLBACK (on_flip_response), fd);
  adw_dialog_present (dialog, GTK_WIDGET (self));
}

typedef struct
{
  EaselWindow *window;
  GtkWidget   *rows[4];
  GtkWidget   *lock;
} StretchDialog;

/* With the lock on, the two stretch fields move together. */
static void
on_stretch_value_changed (AdwSpinRow *row, GParamSpec *pspec, StretchDialog *sd)
{
  if (!adw_switch_row_get_active (ADW_SWITCH_ROW (sd->lock)))
    return;
  AdwSpinRow *other = ADW_SPIN_ROW (GTK_WIDGET (row) == sd->rows[0] ? sd->rows[1] : sd->rows[0]);
  adw_spin_row_set_value (other, adw_spin_row_get_value (row));
}

static void
on_stretch_response (AdwAlertDialog *dialog, const char *response, StretchDialog *sd)
{
  if (g_str_equal (response, "ok"))
    easel_canvas_stretch_skew (sd->window->canvas,
                               (int) adw_spin_row_get_value (ADW_SPIN_ROW (sd->rows[0])),
                               (int) adw_spin_row_get_value (ADW_SPIN_ROW (sd->rows[1])),
                               adw_spin_row_get_value (ADW_SPIN_ROW (sd->rows[2])),
                               adw_spin_row_get_value (ADW_SPIN_ROW (sd->rows[3])));
  g_free (sd);
}

static void
action_stretch_skew (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselWindow *self = user_data;
  static const struct { const char *title; double min, max, value; } rows[4] = {
    { "Stretch horizontally (%)", 1, 500, 100 }, { "Stretch vertically (%)", 1, 500, 100 },
    { "Skew horizontally (°)", -89, 89, 0 }, { "Skew vertically (°)", -89, 89, 0 },
  };
  StretchDialog *sd = g_new0 (StretchDialog, 1);
  GtkWidget *stretch_group = adw_preferences_group_new ();
  GtkWidget *skew_group = adw_preferences_group_new ();
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  AdwDialog *dialog = adw_alert_dialog_new ("Stretch and Skew", NULL);

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (stretch_group), "Stretch");
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (skew_group), "Skew");
  gtk_box_append (GTK_BOX (content), stretch_group);
  gtk_box_append (GTK_BOX (content), skew_group);

  sd->window = self;
  sd->lock = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (sd->lock), "Keep aspect ratio");
  adw_switch_row_set_active (ADW_SWITCH_ROW (sd->lock), TRUE);
  for (int i = 0; i < 4; i++)
    {
      sd->rows[i] = adw_spin_row_new_with_range (rows[i].min, rows[i].max, 1);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (sd->rows[i]), rows[i].title);
      adw_spin_row_set_value (ADW_SPIN_ROW (sd->rows[i]), rows[i].value);
      adw_preferences_group_add (ADW_PREFERENCES_GROUP (i < 2 ? stretch_group : skew_group), sd->rows[i]);
      if (i == 1) /* the lock belongs with the stretch rows */
        adw_preferences_group_add (ADW_PREFERENCES_GROUP (stretch_group), sd->lock);
    }
  g_signal_connect (sd->rows[0], "notify::value", G_CALLBACK (on_stretch_value_changed), sd);
  g_signal_connect (sd->rows[1], "notify::value", G_CALLBACK (on_stretch_value_changed), sd);

  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), content);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog), "cancel", "_Cancel", "ok", "_OK", NULL);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "ok");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_signal_connect (dialog, "response", G_CALLBACK (on_stretch_response), sd);
  adw_dialog_present (dialog, GTK_WIDGET (self));
}


/* ---- View: thumbnail -------------------------------------------------- */

#define THUMB_MAX 240

static void
draw_thumbnail (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  EaselWindow *self = data;
  EaselDocument *doc = easel_canvas_get_document (self->canvas);
  double scale = MIN ((double) width / doc->width, (double) height / doc->height);
  double w = doc->width * scale, h = doc->height * scale;
  double x = (width - w) / 2, y = (height - h) / 2;

  /* The surface wraps the picture's own pixels: no copy. */
  cairo_surface_t *surface = cairo_image_surface_create_for_data ((unsigned char *) doc->pixels, CAIRO_FORMAT_ARGB32,
                                                                  doc->width, doc->height, doc->width * 4);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_rectangle (cr, x, y, w, h);
  cairo_fill (cr);
  cairo_translate (cr, x, y);
  cairo_scale (cr, scale, scale);
  cairo_set_source_surface (cr, surface, 0, 0);
  cairo_pattern_set_filter (cairo_get_source (cr), scale < 1 ? CAIRO_FILTER_GOOD : CAIRO_FILTER_NEAREST);
  cairo_paint (cr);
  cairo_surface_destroy (surface);
}

/* Follows the picture while the window is open (it may be edited or replaced). */
static gboolean
thumb_tick (gpointer data)
{
  EaselWindow *self = data;
  EaselDocument *doc = easel_canvas_get_document (self->canvas);
  double scale = MIN ((double) THUMB_MAX / doc->width, (double) THUMB_MAX / doc->height);
  int w = MAX (1, (int) (doc->width * MIN (scale, 1.0))), h = MAX (1, (int) (doc->height * MIN (scale, 1.0)));

  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (self->thumb_area), w);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (self->thumb_area), h);
  gtk_widget_queue_draw (self->thumb_area);
  return G_SOURCE_CONTINUE;
}

static gboolean
on_thumb_close (GtkWindow *window, EaselWindow *self)
{
  GAction *action = g_action_map_lookup_action (G_ACTION_MAP (self), "show-thumbnail");

  g_clear_handle_id (&self->thumb_timer, g_source_remove);
  gtk_widget_set_visible (GTK_WIDGET (window), FALSE);
  g_simple_action_set_state (G_SIMPLE_ACTION (action), g_variant_new_boolean (FALSE));
  return TRUE;
}

static void
on_thumbnail_change_state (GSimpleAction *action, GVariant *value, gpointer user_data)
{
  EaselWindow *self = user_data;

  if (g_variant_get_boolean (value))
    {
      if (self->thumb_window == NULL)
        {
          self->thumb_area = gtk_drawing_area_new ();
          gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self->thumb_area), draw_thumbnail, self, NULL);
          self->thumb_window = gtk_window_new ();
          gtk_window_set_title (GTK_WINDOW (self->thumb_window), "Thumbnail");
          gtk_window_set_transient_for (GTK_WINDOW (self->thumb_window), GTK_WINDOW (self));
          gtk_window_set_resizable (GTK_WINDOW (self->thumb_window), FALSE);
          gtk_window_set_child (GTK_WINDOW (self->thumb_window), self->thumb_area);
          g_signal_connect (self->thumb_window, "close-request", G_CALLBACK (on_thumb_close), self);
        }
      thumb_tick (self);
      if (self->thumb_timer == 0)
        self->thumb_timer = g_timeout_add (250, thumb_tick, self);
      gtk_window_present (GTK_WINDOW (self->thumb_window));
    }
  else if (self->thumb_window != NULL)
    {
      g_clear_handle_id (&self->thumb_timer, g_source_remove);
      gtk_widget_set_visible (self->thumb_window, FALSE);
    }
  g_simple_action_set_state (action, value);
}

static const GActionEntry window_actions[] = {
  { "swap-colors", action_swap_colors },
  { "clear-image", action_clear_image },
  { "new", action_new },
  { "open", action_open },
  { "save", action_save },
  { "save-as", action_save_as },
  { "attributes", action_attributes },
  { "flip-rotate", action_flip_rotate },
  { "print", action_print },
  { "stretch-skew", action_stretch_skew },
  { "invert-colors", action_invert_colors },
  { "edit-colors", action_edit_colors },
  { "zoom-normal", action_zoom_normal },
  { "zoom-large", action_zoom_large },
  { "zoom-in", action_zoom_in },
  { "zoom-out", action_zoom_out },
  { "zoom-custom", action_zoom_custom },
  { "cut", action_cut },
  { "copy", action_copy },
  { "paste", action_paste },
  { "copy-to", action_copy_to },
  { "paste-from", action_paste_from },
  { "clear-selection", action_clear_selection },
  { "select-all", action_select_all },
  { "undo", action_undo },
  { "redo", action_redo },
};

/* View > Tool Box / Color Box / Status Bar: a stateful action bound straight
 * to the widget's "visible" property. */
static void
add_visibility_action (EaselWindow *self, const char *name, GtkWidget *widget)
{
  GPropertyAction *action = g_property_action_new (name, widget, "visible");
  g_action_map_add_action (G_ACTION_MAP (self), G_ACTION (action));
  g_object_unref (action);
}

/* ---- View > Frame Time ------------------------------------------------ */

static gboolean
frame_tick (gpointer data)
{
  EaselWindow *self = data;
  GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (self->canvas));
  double fps = clock != NULL ? gdk_frame_clock_get_fps (clock) : 0;
  g_autofree char *text = g_strdup_printf ("Paint %.2f ms · %.0f fps",
                                           easel_canvas_get_paint_time (self->canvas) / 1000.0, fps);

  gtk_label_set_text (self->frame_label, text);
  return G_SOURCE_CONTINUE;
}

static void
on_frame_visible (GtkWidget *label, GParamSpec *pspec, EaselWindow *self)
{
  g_clear_handle_id (&self->frame_timer, g_source_remove);
  if (gtk_widget_get_visible (label))
    {
      frame_tick (self);
      self->frame_timer = g_timeout_add (500, frame_tick, self);
    }
}

static GtkWidget *
make_zoom_row (EaselWindow *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *out = gtk_button_new_from_icon_name ("zoom-out-symbolic");
  GtkWidget *in = gtk_button_new_from_icon_name ("zoom-in-symbolic");

  gtk_actionable_set_action_name (GTK_ACTIONABLE (out), "win.zoom-out");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (in), "win.zoom-in");
  gtk_widget_add_css_class (out, "flat");
  gtk_widget_add_css_class (in, "flat");
  gtk_widget_set_tooltip_text (out, "Zoom out");
  gtk_widget_set_tooltip_text (in, "Zoom in");

  self->menu_zoom_label = GTK_LABEL (gtk_label_new ("100%"));
  gtk_widget_add_css_class (GTK_WIDGET (self->menu_zoom_label), "numeric");
  gtk_widget_set_hexpand (GTK_WIDGET (self->menu_zoom_label), TRUE);
  gtk_label_set_width_chars (self->menu_zoom_label, 6);
  g_object_add_weak_pointer (G_OBJECT (self->menu_zoom_label), (gpointer *) &self->menu_zoom_label);

  gtk_box_append (GTK_BOX (box), out);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->menu_zoom_label));
  gtk_box_append (GTK_BOX (box), in);
  return box;
}

static GtkWidget *
build_menu_button (EaselWindow *self)
{
  g_autoptr (GtkBuilder) builder =
    gtk_builder_new_from_resource ("/io/github/jackal_kr/Easel/menus.ui");
  GMenuModel *model = G_MENU_MODEL (gtk_builder_get_object (builder, "primary_menu"));
  GtkWidget *popover = gtk_popover_menu_new_from_model (model);

  GtkWidget *button = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (button), "open-menu-symbolic");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), popover);
  gtk_widget_set_tooltip_text (button, "Main Menu");
  return button;
}

/* A header-bar button with an icon, a label and a drop-down menu. */
static GtkWidget *
build_labeled_menu (EaselWindow *self, const char *menu_id, const char *label, const char *icon,
                    gboolean with_zoom)
{
  g_autoptr (GtkBuilder) builder =
    gtk_builder_new_from_resource ("/io/github/jackal_kr/Easel/menus.ui");
  GMenuModel *model = G_MENU_MODEL (gtk_builder_get_object (builder, menu_id));
  GtkWidget *popover = gtk_popover_menu_new_from_model (model);
  GtkWidget *content = adw_button_content_new ();

  if (with_zoom)
    gtk_popover_menu_add_child (GTK_POPOVER_MENU (popover), make_zoom_row (self), "zoom");
  adw_button_content_set_label (ADW_BUTTON_CONTENT (content), label);
  adw_button_content_set_icon_name (ADW_BUTTON_CONTENT (content), icon);

  GtkWidget *button = gtk_menu_button_new ();
  gtk_menu_button_set_child (GTK_MENU_BUTTON (button), content);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), popover);
  gtk_menu_button_set_always_show_arrow (GTK_MENU_BUTTON (button), TRUE);
  return button;
}

static GtkWidget *
make_header_button (const char *icon, const char *action, const char *tooltip)
{
  GtkWidget *b = gtk_button_new_from_icon_name (icon);
  gtk_actionable_set_action_name (GTK_ACTIONABLE (b), action);
  gtk_widget_set_tooltip_text (b, tooltip);
  return b;
}

static GtkWidget *
build_status_bar (EaselWindow *self, const EaselDocument *doc)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start (bar, 8);
  gtk_widget_set_margin_end (bar, 8);
  gtk_widget_set_margin_top (bar, 4);
  gtk_widget_set_margin_bottom (bar, 4);

  self->hint_label = GTK_LABEL (gtk_label_new (easel_toolbox_get_tool_hint (self->toolbox)));
  gtk_label_set_xalign (self->hint_label, 0);
  gtk_label_set_ellipsize (self->hint_label, PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (GTK_WIDGET (self->hint_label), TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->hint_label), "caption");
  gtk_box_append (GTK_BOX (bar), GTK_WIDGET (self->hint_label));

  self->position_label = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_width_chars (self->position_label, 10);
  gtk_widget_add_css_class (GTK_WIDGET (self->position_label), "caption");
  gtk_widget_add_css_class (GTK_WIDGET (self->position_label), "numeric");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->position_label), "Pointer position");
  gtk_box_append (GTK_BOX (bar), GTK_WIDGET (self->position_label));

  g_autofree char *size_text = g_strdup_printf ("%d × %d px", doc->width, doc->height);
  self->zoom_label = GTK_LABEL (gtk_label_new ("100%"));
  gtk_label_set_width_chars (self->zoom_label, 5);
  gtk_widget_add_css_class (GTK_WIDGET (self->zoom_label), "caption");
  gtk_widget_add_css_class (GTK_WIDGET (self->zoom_label), "numeric");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->zoom_label), "Zoom");
  gtk_box_append (GTK_BOX (bar), GTK_WIDGET (self->zoom_label));

  self->size_label = GTK_LABEL (gtk_label_new (size_text));
  gtk_widget_add_css_class (GTK_WIDGET (self->size_label), "caption");
  gtk_widget_add_css_class (GTK_WIDGET (self->size_label), "numeric");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->size_label), "Image size");
  gtk_box_append (GTK_BOX (bar), GTK_WIDGET (self->size_label));

  self->frame_label = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_width_chars (self->frame_label, 22);
  gtk_widget_add_css_class (GTK_WIDGET (self->frame_label), "caption");
  gtk_widget_add_css_class (GTK_WIDGET (self->frame_label), "numeric");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->frame_label), "Time to paint the last frame, and frames per second while drawing");
  gtk_widget_set_visible (GTK_WIDGET (self->frame_label), FALSE);
  g_signal_connect (self->frame_label, "notify::visible", G_CALLBACK (on_frame_visible), self);
  gtk_box_append (GTK_BOX (bar), GTK_WIDGET (self->frame_label));

  return bar;
}

static void
easel_window_dispose (GObject *object)
{
  EaselWindow *self = EASEL_WINDOW (object);
  g_clear_handle_id (&self->thumb_timer, g_source_remove);
  g_clear_handle_id (&self->frame_timer, g_source_remove);
  if (self->thumb_window != NULL)
    gtk_window_destroy (GTK_WINDOW (self->thumb_window));
  self->thumb_window = NULL;
  g_clear_object (&self->file);
  G_OBJECT_CLASS (easel_window_parent_class)->dispose (object);
}

static void
easel_window_class_init (EaselWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = easel_window_dispose;
}

static void
easel_window_init (EaselWindow *self)
{
  /* Big enough to show the whole default picture with its handles, toolbox,
   * colour box and status bar; shrunk only if the screen cannot hold that. */
  int want_w = DEFAULT_IMAGE_WIDTH + 220, want_h = DEFAULT_IMAGE_HEIGHT + 240;
  GListModel *monitors = gdk_display_get_monitors (gdk_display_get_default ());
  GdkMonitor *monitor = monitors != NULL ? g_list_model_get_item (monitors, 0) : NULL;

  if (monitor != NULL)
    {
      GdkRectangle geo;
      gdk_monitor_get_geometry (monitor, &geo);
      want_w = MIN (want_w, geo.width * 9 / 10);
      want_h = MIN (want_h, geo.height * 85 / 100);
      g_object_unref (monitor);
    }
  gtk_window_set_default_size (GTK_WINDOW (self), want_w, want_h);
  gtk_window_set_title (GTK_WINDOW (self), "Untitled - Easel");

  EaselDocument *doc = easel_document_new (DEFAULT_IMAGE_WIDTH, DEFAULT_IMAGE_HEIGHT, default_background ());
  if (doc == NULL)
    g_error ("out of memory creating the initial image");

  /* Center: tool box | workspace with canvas */
  self->toolbox = EASEL_TOOLBOX (easel_toolbox_new ());
  self->canvas = EASEL_CANVAS (easel_canvas_new (doc)); /* canvas owns doc */
  self->colorbox = EASEL_COLOR_BOX (easel_color_box_new ());

  GtkWidget *workspace = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (workspace, "easel-workspace");
  gtk_widget_set_hexpand (workspace, TRUE);
  gtk_widget_set_vexpand (workspace, TRUE);
  gtk_widget_set_halign (GTK_WIDGET (self->canvas), GTK_ALIGN_START);
  gtk_widget_set_valign (GTK_WIDGET (self->canvas), GTK_ALIGN_START);
  gtk_widget_set_margin_start (GTK_WIDGET (self->canvas), 12);
  gtk_widget_set_margin_end (GTK_WIDGET (self->canvas), 12);
  gtk_widget_set_margin_top (GTK_WIDGET (self->canvas), 12);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self->canvas), 12);
  gtk_box_append (GTK_BOX (workspace), GTK_WIDGET (self->canvas));

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), workspace);
  gtk_widget_set_hexpand (scroller, TRUE);
  gtk_widget_set_vexpand (scroller, TRUE);

  GtkWidget *middle = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (middle), GTK_WIDGET (self->toolbox));
  gtk_box_append (GTK_BOX (middle), gtk_separator_new (GTK_ORIENTATION_VERTICAL));
  gtk_box_append (GTK_BOX (middle), scroller);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (content), middle);
  gtk_box_append (GTK_BOX (content), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->colorbox));

  self->status_bar = build_status_bar (self, doc);

  /* Header bar with the main menu, then everything in an AdwToolbarView. */
  AdwHeaderBar *header = ADW_HEADER_BAR (adw_header_bar_new ());
  adw_header_bar_pack_end (header, build_menu_button (self));
  adw_header_bar_pack_end (header, build_labeled_menu (self, "view_menu", "View", "zoom-fit-best-symbolic", TRUE));
  adw_header_bar_pack_end (header, build_labeled_menu (self, "image_menu", "Image", "image-x-generic-symbolic", FALSE));
  adw_header_bar_pack_start (header, make_header_button ("document-new-symbolic", "win.new", "New (Ctrl+N)"));
  adw_header_bar_pack_start (header, make_header_button ("document-open-symbolic", "win.open", "Open (Ctrl+O)"));
  adw_header_bar_pack_start (header, make_header_button ("document-save-symbolic", "win.save", "Save (Ctrl+S)"));
  GtkWidget *history_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (history_box, "linked");
  gtk_box_append (GTK_BOX (history_box), make_header_button ("edit-undo-symbolic", "win.undo", "Undo (Ctrl+Z)"));
  gtk_box_append (GTK_BOX (history_box), make_header_button ("edit-redo-symbolic", "win.redo", "Redo (Ctrl+Y)"));
  adw_header_bar_pack_start (header, history_box);

  GtkWidget *view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (view), GTK_WIDGET (header));
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (view), content);
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (view), self->status_bar);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self), view);

  /* Wiring */
  g_signal_connect (self->canvas, "pointer-moved", G_CALLBACK (on_pointer_moved), self);
  g_signal_connect (self->colorbox, "colors-changed", G_CALLBACK (on_colors_changed), self);
  g_signal_connect (self->toolbox, "tool-changed", G_CALLBACK (on_tool_changed), self);
  g_signal_connect (self->toolbox, "options-changed", G_CALLBACK (on_options_changed), self);
  g_signal_connect (self->canvas, "color-picked", G_CALLBACK (on_color_picked), self);

  g_action_map_add_action_entries (G_ACTION_MAP (self), window_actions,
                                   G_N_ELEMENTS (window_actions), self);
  add_visibility_action (self, "show-toolbox", GTK_WIDGET (self->toolbox));
  add_visibility_action (self, "show-colorbox", GTK_WIDGET (self->colorbox));
  add_visibility_action (self, "show-statusbar", self->status_bar);
  add_visibility_action (self, "show-frame-time", GTK_WIDGET (self->frame_label));
  if (easel_settings () != NULL)
    {
      /* The panels keep their state between runs. */
      g_settings_bind (easel_settings (), "show-toolbox", self->toolbox, "visible", G_SETTINGS_BIND_DEFAULT);
      g_settings_bind (easel_settings (), "show-colorbox", self->colorbox, "visible", G_SETTINGS_BIND_DEFAULT);
      g_settings_bind (easel_settings (), "show-statusbar", self->status_bar, "visible", G_SETTINGS_BIND_DEFAULT);
      g_settings_bind (easel_settings (), "show-frame-time", self->frame_label, "visible", G_SETTINGS_BIND_DEFAULT);
    }

  g_signal_connect (self, "close-request", G_CALLBACK (on_close_request), NULL);
  GSimpleAction *thumb = g_simple_action_new_stateful ("show-thumbnail", NULL, g_variant_new_boolean (FALSE));
  g_signal_connect (thumb, "change-state", G_CALLBACK (on_thumbnail_change_state), self);
  g_action_map_add_action (G_ACTION_MAP (self), G_ACTION (thumb));
  g_object_unref (thumb);
  GSimpleAction *grid = g_simple_action_new_stateful ("show-grid", NULL, g_variant_new_boolean (FALSE));
  g_signal_connect (grid, "change-state", G_CALLBACK (on_grid_change_state), self);
  g_action_map_add_action (G_ACTION_MAP (self), G_ACTION (grid));
  g_object_unref (grid);
  g_signal_connect (self->canvas, "selection-changed", G_CALLBACK (on_selection_changed), self);
  g_signal_connect (self->canvas, "text-state", G_CALLBACK (on_text_state), self);
  g_signal_connect (self->toolbox, "text-style-toggled", G_CALLBACK (on_text_style_toggled), self);
  on_selection_changed (self->canvas, self);
  g_signal_connect (self->canvas, "zoom-changed", G_CALLBACK (on_zoom_changed), self);
  on_zoom_changed (self->canvas, self);
  g_signal_connect (self->canvas, "resize-preview", G_CALLBACK (on_resize_preview), self);
  g_signal_connect (self->canvas, "history-changed", G_CALLBACK (on_history_changed), self);
  on_history_changed (self->canvas, self);
  set_modified (self, FALSE);

  on_colors_changed (self->colorbox, self);
  sync_canvas_tool (self);
}

EaselWindow *
easel_window_new (GtkApplication *app)
{
  return g_object_new (EASEL_TYPE_WINDOW, "application", app, NULL);
}
