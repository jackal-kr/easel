#include "ui/window.h"
#include <glib/gi18n.h>

#include <math.h>

#include "core/document.h"
#include "ui/canvas.h"
#include "ui/colorbox.h"
#include "ui/config.h"
#include "ui/dialogs.h"
#include "ui/fontdialog.h"
#include "ui/print.h"
#include "ui/fileio.h"
#include "ui/settings.h"
#include "ui/toolbox.h"


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
  guint64    thumb_changes;  /* canvas change count at the last thumbnail redraw */
  int        thumb_w, thumb_h;

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
on_text_color_picked (EaselToolbox *toolbox, guint argb, EaselWindow *self)
{
  easel_canvas_text_set_color (self->canvas, argb);
}

static void
on_colors_changed (EaselColorBox *box, EaselWindow *self)
{
  easel_toolbox_set_foreground (self->toolbox, easel_color_box_get_foreground (box));
  easel_canvas_set_foreground (self->canvas, easel_color_box_get_foreground (box));
  easel_canvas_set_background (self->canvas, easel_color_box_get_background (box));
}

/* The toolbox and the colour box hold the settings, the canvas does the
 * drawing. This copies the current tool and its options over to the canvas. */
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
  easel_canvas_set_eraser_opaque (self->canvas, easel_toolbox_get_eraser_opaque (self->toolbox));
  easel_canvas_set_font (self->canvas, easel_toolbox_get_font (self->toolbox));
  easel_canvas_set_text_opaque (self->canvas, easel_toolbox_get_text_opaque (self->toolbox));
  easel_canvas_set_text_smooth (self->canvas, easel_toolbox_get_text_smooth (self->toolbox));
  easel_canvas_set_gradient_style (self->canvas, easel_toolbox_get_gradient_style (self->toolbox));
}

/* What a screen reader says about the picture: its size, zoom and the tool in hand. */
static void
update_canvas_a11y (EaselWindow *self)
{
  EaselDocument *doc = easel_canvas_get_document (self->canvas);
  g_autofree char *text = g_strdup_printf (_("%d × %d pixels, zoom %d%%, tool: %s"), doc->width, doc->height,
                                           easel_canvas_get_zoom (self->canvas),
                                           _(easel_tool_info (easel_toolbox_get_tool (self->toolbox))->name));

  gtk_accessible_update_property (GTK_ACCESSIBLE (self->canvas), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Picture"),
                                  GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, text, -1);
}

static void
on_tool_changed (EaselToolbox *toolbox, EaselWindow *self)
{
  gtk_label_set_text (self->hint_label, easel_toolbox_get_tool_hint (toolbox));
  sync_canvas_tool (self);
  update_canvas_a11y (self);
  if (gtk_widget_get_mapped (GTK_WIDGET (self)))
    {
      g_autofree char *said = g_strdup_printf (_("Tool: %s"),
                                               _(easel_tool_info (easel_toolbox_get_tool (toolbox))->name));
      gtk_accessible_announce (GTK_ACCESSIBLE (self->hint_label), said, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_LOW);
    }
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

/* Like the original: Clear Image fills everything with the background colour. */
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
  g_autofree char *name = self->file != NULL ? g_file_get_basename (self->file) : g_strdup (_("Untitled"));
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
  g_autofree char *text = g_strdup_printf (_("%d × %d px"), doc->width, doc->height);

  gtk_label_set_text (self->size_label, text);
}

static void
on_history_changed (EaselCanvas *canvas, EaselWindow *self)
{
  set_modified (self, !easel_canvas_is_saved (canvas));
  update_size_label (self);
  update_canvas_a11y (self);
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

  gtk_file_filter_set_name (pictures, _("Pictures"));
  gtk_file_filter_add_mime_type (pictures, "image/png");
  gtk_file_filter_add_mime_type (pictures, "image/jpeg");
  gtk_file_filter_add_mime_type (pictures, "image/bmp");
  gtk_file_filter_add_mime_type (pictures, "image/tiff");
  gtk_file_filter_add_mime_type (pictures, "image/webp");
  gtk_file_filter_add_mime_type (pictures, "image/gif"); /* open only */
  g_list_store_append (filters, pictures);
  g_object_unref (pictures);

  static const struct { const char *name, *mime; } each[] = {
    { "PNG", "image/png" }, { "JPEG", "image/jpeg" }, { "BMP", "image/bmp" }, { "TIFF", "image/tiff" },
    { "WebP", "image/webp" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (each); i++)
    {
      if (g_str_equal (each[i].name, "WebP") && !easel_file_can_write ("webp"))
        continue;
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

  /* Whatever is still floating (selection, text) must be in the pixels
   * before we write them to disk. */
  easel_canvas_flush (self->canvas);
  if (!easel_file_save (easel_canvas_get_document (self->canvas), file, &error))
    {
      show_error (self, _("Could not save the picture"), error);
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

  gtk_file_dialog_set_title (dialog, _("Save As"));
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
  /* Text being typed, a floating selection or an unfinished curve only count
   * as an edit once they are put down. Do that first, otherwise we'd say
   * "nothing changed" and silently throw them away. */
  easel_canvas_flush (self->canvas);

  if (!self->modified)
    {
      next (self);
      return;
    }

  g_autofree char *name = self->file != NULL ? g_file_get_basename (self->file) : g_strdup (_("Untitled"));
  g_autofree char *body = g_strdup_printf (_("Save changes to “%s” before closing?"), name);
  AdwDialog *dialog = adw_alert_dialog_new (_("Save Changes?"), body);

  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                  "cancel", _("_Cancel"), "discard", _("_Discard"), "save", _("_Save"), NULL);
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
  EaselDocument *doc = easel_document_new (easel_default_width (), easel_default_height (), easel_default_background ());
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

  gtk_file_dialog_set_title (dialog, _("Open"));
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
    show_error (self, _("Could not open the picture"), error);
  else
    show_document (self, doc, file);
}

/* easel_window_open_file may have to ask "save changes?" first. The answer
 * comes back later through a callback that can't carry arguments, so the file
 * waits here until then. */
static GFile *pending_open_file;

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

  if (self->closing)
    return FALSE;
  /* Same as in confirm_discard(): settle floating things before deciding if
   * the picture was modified. */
  easel_canvas_flush (self->canvas);
  if (!self->modified)
    return FALSE;
  confirm_discard (self, close_confirmed);
  return TRUE; /* hold the window until the user has answered */
}


static void
on_resize_preview (EaselCanvas *canvas, int w, int h, EaselWindow *self)
{
  if (w < 0)
    {
      update_size_label (self);
      return;
    }
  g_autofree char *text = g_strdup_printf (_("%d × %d px"), w, h);
  gtk_label_set_text (self->size_label, text);
}


/* ---- View > Zoom, Grid ---------------------------------------------- */

static void set_action_enabled (EaselWindow *self, const char *name, gboolean enabled);

static void
on_zoom_changed (EaselCanvas *canvas, EaselWindow *self)
{
  g_autofree char *text = g_strdup_printf ("%d%%", easel_canvas_get_zoom (canvas));
  gtk_label_set_text (self->zoom_label, text);
  update_canvas_a11y (self);
  if (self->menu_zoom_label != NULL)
    gtk_label_set_text (self->menu_zoom_label, text);
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
action_zoom_fit (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_canvas_zoom_fit (((EaselWindow *) user_data)->canvas);
}

static void
action_grid (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_dialog_grid (GTK_WIDGET (user_data), ((EaselWindow *) user_data)->canvas);
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
            show_error (self, _("Could not save the selection"), error);
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

  gtk_file_dialog_set_title (dialog, _("Copy To"));
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
        show_error (self, _("Could not open the picture"), error);
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

  gtk_file_dialog_set_title (dialog, _("Paste From"));
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

typedef struct { EaselWindow *window; EaselClip *clip; } OversizePaste;

static void
on_oversize_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
  OversizePaste *op = data;

  if (g_str_equal (response, "fit"))
    easel_canvas_paste_clip_as (op->window->canvas, op->clip, TRUE);
  else if (g_str_equal (response, "resize"))
    easel_canvas_paste_clip_as (op->window->canvas, op->clip, FALSE);
  else
    easel_clip_free (op->clip);
  g_object_unref (op->window);
  g_free (op);
}

/* The pasted picture is bigger than the canvas: shrink it, or grow the canvas. */
static void
on_paste_oversize (EaselCanvas *canvas, EaselClip *clip, gpointer data)
{
  EaselWindow *self = data;
  OversizePaste *op = g_new0 (OversizePaste, 1);
  g_autofree char *body = g_strdup_printf (_("The picture is %d × %d pixels; the canvas is %d × %d."),
                                           clip->width, clip->height, easel_canvas_get_document (canvas)->width,
                                           easel_canvas_get_document (canvas)->height);
  AdwDialog *dialog = adw_alert_dialog_new (_("Pasted picture is larger than the canvas"), body);

  op->window = g_object_ref (self);
  op->clip = clip;
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog), "cancel", _("_Cancel"), "fit",
                                  _("Fit Picture to Canvas"), "resize", _("Resize Canvas to Picture"), NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "resize", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "resize");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_signal_connect (dialog, "response", G_CALLBACK (on_oversize_response), op);
  adw_dialog_present (dialog, GTK_WIDGET (self));
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

static void
action_turn (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  static const struct { const char *name; EaselTurn turn; } turns[] = {
    { "flip-horizontal", EASEL_FLIP_H }, { "flip-vertical", EASEL_FLIP_V }, { "rotate-90", EASEL_ROTATE_90 },
    { "rotate-180", EASEL_ROTATE_180 }, { "rotate-270", EASEL_ROTATE_270 },
  };
  const char *name = g_action_get_name (G_ACTION (action));

  for (guint i = 0; i < G_N_ELEMENTS (turns); i++)
    if (g_str_equal (name, turns[i].name))
      easel_canvas_turn (((EaselWindow *) user_data)->canvas, turns[i].turn);
}

/* Text box context menu: the same editing as the keyboard, on the text. */
static void
action_text_edit (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselCanvas *canvas = ((EaselWindow *) user_data)->canvas;
  const char *name = g_action_get_name (G_ACTION (action));

  if (g_str_equal (name, "text-cut"))
    easel_canvas_text_cut (canvas);
  else if (g_str_equal (name, "text-copy"))
    easel_canvas_text_copy (canvas);
  else if (g_str_equal (name, "text-paste"))
    easel_canvas_text_paste (canvas);
  else if (g_str_equal (name, "text-delete"))
    easel_canvas_text_delete (canvas);
  else
    easel_canvas_text_select_all (canvas);
}

/* "Font for Selection...": the picker applies to the selected text only (or to
 * what is typed next when nothing is selected), not to the whole box. */
static void
on_selection_font_picked (const char *font, guint flags, uint32_t argb, gboolean color_changed, gpointer data)
{
  EaselWindow *self = data;
  static const guint all[4] = { EASEL_TEXT_BOLD, EASEL_TEXT_ITALIC, EASEL_TEXT_UNDERLINE, EASEL_TEXT_STRIKE };

  easel_canvas_text_set_font (self->canvas, font);
  if (color_changed)
    easel_canvas_text_set_color (self->canvas, argb);
  for (int i = 0; i < 4; i++)
    easel_canvas_text_apply_style (self->canvas, all[i], (flags & all[i]) != 0);
}

static gboolean
on_font_requested (EaselToolbox *toolbox, EaselWindow *self)
{
  if (!easel_canvas_text_has_selection (self->canvas))
    return FALSE;
  gtk_widget_activate_action (GTK_WIDGET (self), "win.text-font", NULL);
  return TRUE;
}

static void
action_text_font (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselWindow *self = user_data;
  guint flags = 0;
  uint32_t argb = 0;
  g_autofree char *font = easel_canvas_text_get_font (self->canvas, &flags, &argb);

  easel_font_dialog_present (GTK_WIDGET (self), font, flags, argb, on_selection_font_picked, self);
}

/* ---- Image and file actions that open a dialog ------------------------ */

static void
action_attributes (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_dialog_attributes (GTK_WIDGET (user_data), ((EaselWindow *) user_data)->canvas);
}

static void
action_zoom_custom (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_dialog_custom_zoom (GTK_WIDGET (user_data), ((EaselWindow *) user_data)->canvas);
}

static void
action_flip_rotate (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_dialog_flip_rotate (GTK_WIDGET (user_data), ((EaselWindow *) user_data)->canvas);
}

static void
action_stretch_skew (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  easel_dialog_stretch_skew (GTK_WIDGET (user_data), ((EaselWindow *) user_data)->canvas);
}

static void
action_print (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
  EaselWindow *self = user_data;

  easel_canvas_flush (self->canvas);
  easel_print_picture (GTK_WINDOW (self), self->canvas);
}

/* ---- View: thumbnail -------------------------------------------------- */

#define THUMB_MAX EASEL_THUMB_MAX

static void
draw_thumbnail (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  EaselWindow *self = data;
  EaselDocument *doc = easel_canvas_get_document (self->canvas);
  double scale = MIN ((double) width / doc->width, (double) height / doc->height);
  double w = doc->width * scale, h = doc->height * scale;
  double x = (width - w) / 2, y = (height - h) / 2;

  /* This Cairo surface just points at the picture's own memory, so making the
   * thumbnail doesn't copy the whole picture first. */
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

/* Timer that keeps the thumbnail window in step with the picture. It's cheap
 * when nothing happened: the canvas bumps a change counter on every edit, so
 * we compare numbers and only redraw when it moved. */
static gboolean
thumb_tick (gpointer data)
{
  EaselWindow *self = data;
  EaselDocument *doc = easel_canvas_get_document (self->canvas);
  double scale = MIN ((double) THUMB_MAX / doc->width, (double) THUMB_MAX / doc->height);
  int w = MAX (1, (int) (doc->width * MIN (scale, 1.0))), h = MAX (1, (int) (doc->height * MIN (scale, 1.0)));
  guint64 changes = easel_canvas_get_change_count (self->canvas);

  /* Nothing to do while the picture is untouched. */
  if (changes == self->thumb_changes && w == self->thumb_w && h == self->thumb_h)
    return G_SOURCE_CONTINUE;
  self->thumb_changes = changes;
  self->thumb_w = w;
  self->thumb_h = h;
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

static gboolean
on_thumb_key (GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer data)
{
  if (keyval != GDK_KEY_Escape)
    return GDK_EVENT_PROPAGATE;
  gtk_window_close (GTK_WINDOW (data));
  return GDK_EVENT_STOP;
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
          gtk_window_set_title (GTK_WINDOW (self->thumb_window), _("Thumbnail"));
          gtk_window_set_transient_for (GTK_WINDOW (self->thumb_window), GTK_WINDOW (self));
          gtk_window_set_resizable (GTK_WINDOW (self->thumb_window), FALSE);
          gtk_window_set_child (GTK_WINDOW (self->thumb_window), self->thumb_area);
          g_signal_connect (self->thumb_window, "close-request", G_CALLBACK (on_thumb_close), self);
          GtkEventController *keys = gtk_event_controller_key_new ();
          g_signal_connect (keys, "key-pressed", G_CALLBACK (on_thumb_key), self->thumb_window);
          gtk_widget_add_controller (self->thumb_window, keys);
        }
      self->thumb_w = 0; /* force the first redraw */
      thumb_tick (self);
      if (self->thumb_timer == 0)
        self->thumb_timer = g_timeout_add (EASEL_THUMB_REFRESH_MS, thumb_tick, self);
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
  { "flip-horizontal", action_turn },
  { "flip-vertical", action_turn },
  { "rotate-90", action_turn },
  { "rotate-180", action_turn },
  { "rotate-270", action_turn },
  { "text-cut", action_text_edit },
  { "text-copy", action_text_edit },
  { "text-font", action_text_font },
  { "text-paste", action_text_edit },
  { "text-delete", action_text_edit },
  { "text-select-all", action_text_edit },
  { "edit-colors", action_edit_colors },
  { "zoom-normal", action_zoom_normal },
  { "zoom-fit", action_zoom_fit },
  { "grid", action_grid },
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
  g_autofree char *text = g_strdup_printf (_("Paint %.2f ms · %.0f fps"),
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
      self->frame_timer = g_timeout_add (EASEL_FRAME_REFRESH_MS, frame_tick, self);
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
  gtk_widget_set_tooltip_text (out, _("Zoom out"));
  gtk_widget_set_tooltip_text (in, _("Zoom in"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (out), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Zoom out"), -1);
  gtk_accessible_update_property (GTK_ACCESSIBLE (in), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Zoom in"), -1);

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
  gtk_widget_set_tooltip_text (button, _("Main Menu"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Main Menu"), -1);
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
  gtk_accessible_update_property (GTK_ACCESSIBLE (b), GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip, -1);
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
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->position_label), _("Pointer position"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->position_label), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Pointer position"), -1);
  gtk_box_append (GTK_BOX (bar), GTK_WIDGET (self->position_label));

  g_autofree char *size_text = g_strdup_printf (_("%d × %d px"), doc->width, doc->height);
  self->zoom_label = GTK_LABEL (gtk_label_new ("100%"));
  gtk_label_set_width_chars (self->zoom_label, 5);
  gtk_widget_add_css_class (GTK_WIDGET (self->zoom_label), "caption");
  gtk_widget_add_css_class (GTK_WIDGET (self->zoom_label), "numeric");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->zoom_label), _("Zoom"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->zoom_label), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Zoom"), -1);
  gtk_box_append (GTK_BOX (bar), GTK_WIDGET (self->zoom_label));

  self->size_label = GTK_LABEL (gtk_label_new (size_text));
  gtk_widget_add_css_class (GTK_WIDGET (self->size_label), "caption");
  gtk_widget_add_css_class (GTK_WIDGET (self->size_label), "numeric");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->size_label), _("Image size"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->size_label), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Image size"), -1);
  gtk_box_append (GTK_BOX (bar), GTK_WIDGET (self->size_label));

  self->frame_label = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_width_chars (self->frame_label, 22);
  gtk_widget_add_css_class (GTK_WIDGET (self->frame_label), "caption");
  gtk_widget_add_css_class (GTK_WIDGET (self->frame_label), "numeric");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->frame_label), _("Time to paint the last frame, and frames per second while drawing"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->frame_label), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, _("Time to paint the last frame, and frames per second while drawing"), -1);
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
  int want_w = easel_default_width () + 220, want_h = easel_default_height () + 240;
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

  EaselDocument *doc = easel_document_new (easel_default_width (), easel_default_height (), easel_default_background ());
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
  /* GTK scrolls a widget into view when it gets focus. The canvas takes focus
   * back every time a menu closes, which made the picture jump to its corner.
   * Turning scroll-to-focus off on the viewport stops that. */
  GtkWidget *viewport = gtk_widget_get_ancestor (workspace, GTK_TYPE_VIEWPORT);
  if (viewport != NULL)
    gtk_viewport_set_scroll_to_focus (GTK_VIEWPORT (viewport), FALSE);
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
  adw_header_bar_pack_end (header, build_labeled_menu (self, "view_menu", _("View"), "zoom-fit-best-symbolic", TRUE));
  adw_header_bar_pack_end (header, build_labeled_menu (self, "image_menu", _("Image"), "image-x-generic-symbolic", FALSE));
  adw_header_bar_pack_start (header, make_header_button ("document-new-symbolic", "win.new", _("New (Ctrl+N)")));
  adw_header_bar_pack_start (header, make_header_button ("document-open-symbolic", "win.open", _("Open (Ctrl+O)")));
  adw_header_bar_pack_start (header, make_header_button ("document-save-symbolic", "win.save", _("Save (Ctrl+S)")));
  GtkWidget *history_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (history_box, "linked");
  gtk_box_append (GTK_BOX (history_box), make_header_button ("edit-undo-symbolic", "win.undo", _("Undo (Ctrl+Z)")));
  gtk_box_append (GTK_BOX (history_box), make_header_button ("edit-redo-symbolic", "win.redo", _("Redo (Ctrl+Y)")));
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
  g_signal_connect (self->canvas, "selection-changed", G_CALLBACK (on_selection_changed), self);
  g_signal_connect (self->canvas, "text-state", G_CALLBACK (on_text_state), self);
  g_signal_connect (self->toolbox, "text-style-toggled", G_CALLBACK (on_text_style_toggled), self);
  g_signal_connect (self->toolbox, "font-requested", G_CALLBACK (on_font_requested), self);
  g_signal_connect (self->toolbox, "text-color-picked", G_CALLBACK (on_text_color_picked), self);
  on_selection_changed (self->canvas, self);
  {
    g_autoptr (GtkBuilder) menus = gtk_builder_new_from_resource ("/io/github/jackal_kr/Easel/menus.ui");
    easel_canvas_set_context_menus (self->canvas, G_MENU_MODEL (gtk_builder_get_object (menus, "selection_menu")),
                                    G_MENU_MODEL (gtk_builder_get_object (menus, "text_menu")),
                                    G_MENU_MODEL (gtk_builder_get_object (menus, "pan_menu")));
    easel_canvas_set_oversize_handler (self->canvas, on_paste_oversize, self);
  }
  g_signal_connect (self->canvas, "zoom-changed", G_CALLBACK (on_zoom_changed), self);
  on_zoom_changed (self->canvas, self);
  g_signal_connect (self->canvas, "resize-preview", G_CALLBACK (on_resize_preview), self);
  g_signal_connect (self->canvas, "history-changed", G_CALLBACK (on_history_changed), self);
  on_history_changed (self->canvas, self);
  set_modified (self, FALSE);

  on_colors_changed (self->colorbox, self);
  sync_canvas_tool (self);
}

/* Developer aid, used for the store screenshots: run with
 * EASEL_SCREENSHOT=file.png and the window is saved to that file, then the app
 * quits. Does nothing when the variable isn't set. */
static gboolean
take_screenshot (gpointer data)
{
  GtkWidget *widget = data;
  const char *path = g_getenv ("EASEL_SCREENSHOT");
  GdkPaintable *paintable = gtk_widget_paintable_new (widget);
  GtkSnapshot *snapshot = gtk_snapshot_new ();

  gdk_paintable_snapshot (paintable, snapshot, gtk_widget_get_width (widget), gtk_widget_get_height (widget));
  GskRenderNode *node = gtk_snapshot_free_to_node (snapshot);
  GskRenderer *renderer = gtk_native_get_renderer (GTK_NATIVE (widget));

  if (node != NULL && renderer != NULL)
    {
      GdkTexture *texture = gsk_renderer_render_texture (renderer, node, NULL);

      gdk_texture_save_to_png (texture, path);
      g_object_unref (texture);
    }
  if (node != NULL)
    gsk_render_node_unref (node);
  g_object_unref (paintable);
  g_application_quit (G_APPLICATION (gtk_window_get_application (GTK_WINDOW (widget))));
  return G_SOURCE_REMOVE;
}

EaselWindow *
easel_window_new (GtkApplication *app)
{
  EaselWindow *window = g_object_new (EASEL_TYPE_WINDOW, "application", app, NULL);

  if (g_getenv ("EASEL_SCREENSHOT") != NULL)
    g_timeout_add (3000, take_screenshot, window);
  return window;
}
