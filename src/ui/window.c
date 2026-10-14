#include "ui/window.h"

#include <math.h>

#include "core/document.h"
#include "ui/canvas.h"
#include "ui/colorbox.h"
#include "ui/fileio.h"
#include "ui/toolbox.h"

#define DEFAULT_IMAGE_WIDTH  800
#define DEFAULT_IMAGE_HEIGHT 600

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
    easel_canvas_set_zoom (self->canvas, easel_toolbox_get_zoom_level (toolbox), -1, -1);
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
  set_modified (self, TRUE);
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
  g_set_object (&self->file, file);
  update_size_label (self);
  set_modified (self, FALSE);
}

static void
do_new (EaselWindow *self)
{
  EaselDocument *old = easel_canvas_get_document (self->canvas);
  EaselDocument *doc = easel_document_new (old->width, old->height,
                                           easel_color_box_get_background (self->colorbox));
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
  AdwComboRow *units_row;
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
                             "New area is filled with the background color; a smaller size crops the picture.");
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

static void
on_zoom_changed (EaselCanvas *canvas, EaselWindow *self)
{
  g_autofree char *text = g_strdup_printf ("%d%%", easel_canvas_get_zoom (canvas) * 100);
  gtk_label_set_text (self->zoom_label, text);
}

static void
action_zoom_normal (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_canvas_set_zoom (((EaselWindow *) user_data)->canvas, 1, -1, -1);
}

static void
action_zoom_large (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_canvas_set_zoom (((EaselWindow *) user_data)->canvas, 4, -1, -1);
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
    easel_canvas_set_zoom (self->canvas, zoom_choices[i], -1, -1);
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
    if (zoom_choices[i] == easel_canvas_get_zoom (self->canvas))
      current = i;
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), "Zoom to");
  adw_combo_row_set_model (row, G_LIST_MODEL (gtk_string_list_new (labels)));
  adw_combo_row_set_selected (row, current);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (row));

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
}

static void
action_cut (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_canvas_cut (((EaselWindow *) user_data)->canvas);
}

static void
action_copy (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_canvas_copy (((EaselWindow *) user_data)->canvas);
}

static void
action_clear_selection (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_canvas_clear_selection (((EaselWindow *) user_data)->canvas);
}

static void
action_paste (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselWindow *self = user_data;

  easel_toolbox_select_tool (self->toolbox, EASEL_TOOL_SELECT);
  easel_canvas_paste (self->canvas);
}

static void
action_select_all (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselWindow *self = user_data;

  easel_toolbox_select_tool (self->toolbox, EASEL_TOOL_SELECT);
  easel_canvas_select_all (self->canvas);
}

static const GActionEntry window_actions[] = {
  { "swap-colors", action_swap_colors },
  { "clear-image", action_clear_image },
  { "new", action_new },
  { "open", action_open },
  { "save", action_save },
  { "save-as", action_save_as },
  { "attributes", action_attributes },
  { "zoom-normal", action_zoom_normal },
  { "zoom-large", action_zoom_large },
  { "zoom-custom", action_zoom_custom },
  { "cut", action_cut },
  { "copy", action_copy },
  { "paste", action_paste },
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

static GtkWidget *
build_menu_button (void)
{
  g_autoptr (GtkBuilder) builder =
    gtk_builder_new_from_resource ("/io/github/jackal_kr/Easel/menus.ui");
  GMenuModel *model = G_MENU_MODEL (gtk_builder_get_object (builder, "primary_menu"));

  GtkWidget *button = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (button), "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (button), model);
  gtk_menu_button_set_primary (GTK_MENU_BUTTON (button), TRUE);
  gtk_widget_set_tooltip_text (button, "Main Menu");
  return button;
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

  return bar;
}

static void
easel_window_dispose (GObject *object)
{
  EaselWindow *self = EASEL_WINDOW (object);
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
  gtk_window_set_default_size (GTK_WINDOW (self), 960, 680);
  gtk_window_set_title (GTK_WINDOW (self), "Untitled - Easel");

  EaselDocument *doc = easel_document_new (DEFAULT_IMAGE_WIDTH, DEFAULT_IMAGE_HEIGHT,
                                           EASEL_RGB (255, 255, 255));
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
  adw_header_bar_pack_end (header, build_menu_button ());

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

  g_signal_connect (self, "close-request", G_CALLBACK (on_close_request), NULL);
  GSimpleAction *grid = g_simple_action_new_stateful ("show-grid", NULL, g_variant_new_boolean (FALSE));
  g_signal_connect (grid, "change-state", G_CALLBACK (on_grid_change_state), self);
  g_action_map_add_action (G_ACTION_MAP (self), G_ACTION (grid));
  g_object_unref (grid);
  g_signal_connect (self->canvas, "selection-changed", G_CALLBACK (on_selection_changed), self);
  on_selection_changed (self->canvas, self);
  g_signal_connect (self->canvas, "zoom-changed", G_CALLBACK (on_zoom_changed), self);
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
