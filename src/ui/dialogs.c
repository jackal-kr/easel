#include "ui/dialogs.h"
#include <glib/gi18n.h>

#include <math.h>

#include "ui/config.h"
#include "ui/settings.h"

/* Cancel / OK buttons with OK as the suggested, default answer. */
void
easel_dialog_add_ok_cancel (AdwAlertDialog *dialog)
{
  adw_alert_dialog_add_responses (dialog, "cancel", _("_Cancel"), "ok", _("_OK"), NULL);
  adw_alert_dialog_set_response_appearance (dialog, "ok", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (dialog, "ok");
  adw_alert_dialog_set_close_response (dialog, "cancel");
}

/* ---- Image > Attributes --------------------------------------------- */

typedef struct
{
  EaselCanvas *canvas;
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
  gtk_adjustment_configure (aw, st->px_w / k, 1 / k, EASEL_MAX_DIMENSION / k, st->unit == 0 ? 1 : 0.1, 10, 0);
  gtk_adjustment_configure (ah, st->px_h / k, 1 / k, EASEL_MAX_DIMENSION / k, st->unit == 0 ? 1 : 0.1, 10, 0);
}

static void
attr_read_size (AttrState *st)
{
  double k = px_per_unit (st->unit);

  st->px_w = CLAMP (gtk_adjustment_get_value (adw_spin_row_get_adjustment (st->width_row)) * k, 1, EASEL_MAX_DIMENSION);
  st->px_h = CLAMP (gtk_adjustment_get_value (adw_spin_row_get_adjustment (st->height_row)) * k, 1, EASEL_MAX_DIMENSION);
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
      easel_canvas_resize (st->canvas, (int) lround (st->px_w), (int) lround (st->px_h));
      if (adw_combo_row_get_selected (st->colors_row) == 1)
        easel_canvas_black_and_white (st->canvas);
    }
}

static void
on_attr_default (GtkButton *button, AttrState *st)
{
  st->px_w = easel_default_width ();
  st->px_h = easel_default_height ();
  attr_show_size (st);
}

void
easel_dialog_attributes (GtkWidget *parent, EaselCanvas *canvas)
{
  EaselDocument *doc = easel_canvas_get_document (canvas);
  AttrState *st = g_new0 (AttrState, 1);
  AdwDialog *dialog = adw_alert_dialog_new (_("Attributes"), NULL);

  st->canvas = canvas;
  st->px_w = doc->width;
  st->px_h = doc->height;

  GtkWidget *group = adw_preferences_group_new ();
  st->width_row = ADW_SPIN_ROW (adw_spin_row_new_with_range (1, EASEL_MAX_DIMENSION, 1));
  st->height_row = ADW_SPIN_ROW (adw_spin_row_new_with_range (1, EASEL_MAX_DIMENSION, 1));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (st->width_row), _("Width"));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (st->height_row), _("Height"));

  st->units_row = ADW_COMBO_ROW (adw_combo_row_new ());
  const char *const units[] = { _("Pixels"), _("Inches"), _("Centimeters"), NULL };
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (st->units_row), _("Units"));
  adw_combo_row_set_model (st->units_row, G_LIST_MODEL (gtk_string_list_new (units)));

  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (st->width_row));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (st->height_row));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (st->units_row));

  st->colors_row = ADW_COMBO_ROW (adw_combo_row_new ());
  const char *const colors[] = { _("Colors"), _("Black and white"), NULL };
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (st->colors_row), _("Color mode"));
  adw_combo_row_set_model (st->colors_row, G_LIST_MODEL (gtk_string_list_new (colors)));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (st->colors_row));
  attr_show_size (st);
  g_signal_connect (st->units_row, "notify::selected", G_CALLBACK (on_attr_units_changed), st);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *reset = gtk_button_new_with_mnemonic (_("_Default Size"));
  gtk_widget_set_halign (reset, GTK_ALIGN_END);
  g_signal_connect (reset, "clicked", G_CALLBACK (on_attr_default), st);
  gtk_box_append (GTK_BOX (box), group);
  gtk_box_append (GTK_BOX (box), reset);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), box);
  adw_alert_dialog_set_body (ADW_ALERT_DIALOG (dialog),
                             _("New area is filled with the background color; a smaller size crops the picture. Black and white converts the picture and cannot be reverted except with Undo."));
  easel_dialog_add_ok_cancel (ADW_ALERT_DIALOG (dialog));
  g_signal_connect (dialog, "response", G_CALLBACK (on_attr_response), st);
  g_object_set_data_full (G_OBJECT (dialog), "attr-state", st, g_free);
  adw_dialog_present (dialog, parent);
}


static const int zoom_choices[] = EASEL_MAGNIFIER_LEVELS;

static void
on_zoom_custom_response (AdwAlertDialog *dialog, const char *response, EaselCanvas *canvas)
{
  if (!g_str_equal (response, "ok"))
    return;

  AdwComboRow *row = g_object_get_data (G_OBJECT (dialog), "zoom-row");
  guint i = adw_combo_row_get_selected (row);
  if (i < G_N_ELEMENTS (zoom_choices))
    easel_canvas_set_zoom (canvas, zoom_choices[i] * 100, -1, -1);
}

void
easel_dialog_custom_zoom (GtkWidget *parent, EaselCanvas *canvas)
{
  AdwDialog *dialog = adw_alert_dialog_new (_("Custom Zoom"), NULL);
  AdwComboRow *row = ADW_COMBO_ROW (adw_combo_row_new ());
  const char *const labels[] = { "100%", "200%", "400%", "600%", "800%", NULL };
  GtkWidget *group = adw_preferences_group_new ();
  guint current = 0;

  for (guint i = 0; i < G_N_ELEMENTS (zoom_choices); i++)
    if (zoom_choices[i] * 100 == easel_canvas_get_zoom (canvas))
      current = i;
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _("Zoom to"));
  adw_combo_row_set_model (row, G_LIST_MODEL (gtk_string_list_new (labels)));
  adw_combo_row_set_selected (row, current);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), GTK_WIDGET (row));

  adw_dialog_set_content_width (dialog, 440);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), group);
  easel_dialog_add_ok_cancel (ADW_ALERT_DIALOG (dialog));
  g_object_set_data (G_OBJECT (dialog), "zoom-row", row);
  g_signal_connect (dialog, "response", G_CALLBACK (on_zoom_custom_response), canvas);
  adw_dialog_present (dialog, parent);
}


typedef struct
{
  EaselCanvas *canvas;
  GtkWidget   *choices[5];
} FlipDialog;

static void
on_flip_response (AdwAlertDialog *dialog, const char *response, FlipDialog *fd)
{
  if (g_str_equal (response, "ok"))
    for (int i = 0; i < 5; i++)
      if (gtk_check_button_get_active (GTK_CHECK_BUTTON (fd->choices[i])))
        easel_canvas_turn (fd->canvas, (EaselTurn) i);
  g_free (fd);
}


void
easel_dialog_flip_rotate (GtkWidget *parent, EaselCanvas *canvas)
{
  const char *const labels[5] = {
    _("Flip horizontal"), _("Flip vertical"), _("Rotate 90° clockwise"), _("Rotate 180°"), _("Rotate 270° clockwise"),
  };
  FlipDialog *fd = g_new0 (FlipDialog, 1);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  AdwDialog *dialog = adw_alert_dialog_new (_("Flip and Rotate"), NULL);

  fd->canvas = canvas;
  for (int i = 0; i < 5; i++)
    {
      fd->choices[i] = gtk_check_button_new_with_label (labels[i]);
      if (i > 0)
        gtk_check_button_set_group (GTK_CHECK_BUTTON (fd->choices[i]), GTK_CHECK_BUTTON (fd->choices[0]));
      gtk_box_append (GTK_BOX (box), fd->choices[i]);
    }
  gtk_check_button_set_active (GTK_CHECK_BUTTON (fd->choices[0]), TRUE);

  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), box);
  easel_dialog_add_ok_cancel (ADW_ALERT_DIALOG (dialog));
  g_signal_connect (dialog, "response", G_CALLBACK (on_flip_response), fd);
  adw_dialog_present (dialog, parent);
}

typedef struct
{
  EaselCanvas *canvas;
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
    easel_canvas_stretch_skew (sd->canvas,
                               (int) adw_spin_row_get_value (ADW_SPIN_ROW (sd->rows[0])),
                               (int) adw_spin_row_get_value (ADW_SPIN_ROW (sd->rows[1])),
                               adw_spin_row_get_value (ADW_SPIN_ROW (sd->rows[2])),
                               adw_spin_row_get_value (ADW_SPIN_ROW (sd->rows[3])));
  g_free (sd);
}

void
easel_dialog_stretch_skew (GtkWidget *parent, EaselCanvas *canvas)
{
  const struct { const char *title; double min, max, value; } rows[4] = {
    { _("Stretch horizontally (%)"), 1, 500, 100 }, { _("Stretch vertically (%)"), 1, 500, 100 },
    { _("Skew horizontally (°)"), -89, 89, 0 }, { _("Skew vertically (°)"), -89, 89, 0 },
  };
  StretchDialog *sd = g_new0 (StretchDialog, 1);
  GtkWidget *stretch_group = adw_preferences_group_new ();
  GtkWidget *skew_group = adw_preferences_group_new ();
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  AdwDialog *dialog = adw_alert_dialog_new (_("Stretch and Skew"), NULL);

  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (stretch_group), _("Stretch"));
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (skew_group), _("Skew"));
  gtk_box_append (GTK_BOX (content), stretch_group);
  gtk_box_append (GTK_BOX (content), skew_group);

  sd->canvas = canvas;
  sd->lock = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (sd->lock), _("Keep aspect ratio"));
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
  easel_dialog_add_ok_cancel (ADW_ALERT_DIALOG (dialog));
  g_signal_connect (dialog, "response", G_CALLBACK (on_stretch_response), sd);
  adw_dialog_present (dialog, parent);
}


