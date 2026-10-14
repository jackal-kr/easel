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

/* A number row whose value shows its unit ("100%", "64 px", "0°"). The unit lives in the row's "unit"
 * data, so it can change (percent <-> pixels). */
static void
spin_show (AdwSpinRow *row)
{
  const char *unit = g_object_get_data (G_OBJECT (row), "unit");
  g_autofree char *text = g_strdup_printf ("%.0f%s", adw_spin_row_get_value (row), unit != NULL ? unit : "");

  gtk_editable_set_text (GTK_EDITABLE (row), text);
}

static gboolean
on_spin_output (AdwSpinRow *row, gpointer data)
{
  spin_show (row);
  return TRUE;
}

static int
on_spin_input (AdwSpinRow *row, double *value, gpointer data)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (row));
  char *end = NULL;
  double v = g_ascii_strtod (text, &end);

  /* Nothing typed yet (the row is still being built): keep the value instead of snapping to the minimum. */
  *value = end == text ? adw_spin_row_get_value (row) : v;
  return TRUE;
}

static void
spin_row_set_unit (GtkWidget *row, const char *unit)
{
  g_object_set_data (G_OBJECT (row), "unit", (gpointer) unit);
  if (gtk_widget_get_mapped (row))
    spin_show (ADW_SPIN_ROW (row));
}

static GtkWidget *
unit_spin_row (const char *title, double min, double max, double value, const char *unit)
{
  GtkWidget *row = adw_spin_row_new_with_range (min, max, 1);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_spin_row_set_numeric (ADW_SPIN_ROW (row), FALSE); /* a numeric row refuses the unit text */
  adw_spin_row_set_value (ADW_SPIN_ROW (row), value);
  g_object_set_data (G_OBJECT (row), "unit", (gpointer) unit);
  g_signal_connect (row, "output", G_CALLBACK (on_spin_output), NULL);
  g_signal_connect (row, "input", G_CALLBACK (on_spin_input), NULL);
  return row;
}

typedef struct
{
  EaselCanvas *canvas;
  GtkWidget   *by;         /* "Stretch by": percent or pixels */
  GtkWidget   *rows[2];    /* stretch horizontally, vertically, in the chosen unit */
  GtkWidget   *skew[2];
  GtkWidget   *lock;
  int          base_w, base_h; /* size being stretched, in pixels */
  double       pct[2];     /* the stretch as percent, whatever the display shows */
  gboolean     busy;       /* a field is being updated from another one */
} StretchDialog;

static gboolean
stretch_in_pixels (StretchDialog *sd)
{
  return adw_combo_row_get_selected (ADW_COMBO_ROW (sd->by)) == 1;
}

/* Shows pct[axis] in the row, in percent or pixels. */
static void
stretch_show (StretchDialog *sd, int axis)
{
  int base = axis == 0 ? sd->base_w : sd->base_h;

  adw_spin_row_set_value (ADW_SPIN_ROW (sd->rows[axis]),
                          stretch_in_pixels (sd) ? round (base * sd->pct[axis] / 100.0) : round (sd->pct[axis]));
}

static void
on_stretch_value_changed (AdwSpinRow *row, GParamSpec *pspec, StretchDialog *sd)
{
  if (sd->busy)
    return;
  sd->busy = TRUE;

  int axis = GTK_WIDGET (row) == sd->rows[0] ? 0 : 1;
  int base = axis == 0 ? sd->base_w : sd->base_h;
  double v = adw_spin_row_get_value (row);

  sd->pct[axis] = stretch_in_pixels (sd) ? v * 100.0 / base : v;
  if (adw_switch_row_get_active (ADW_SWITCH_ROW (sd->lock)))
    {
      sd->pct[1 - axis] = sd->pct[axis];
      stretch_show (sd, 1 - axis);
    }
  sd->busy = FALSE;
}

/* Switching unit re-expresses the same stretch in the other unit. */
static void
on_stretch_unit_changed (AdwComboRow *combo, GParamSpec *pspec, StretchDialog *sd)
{
  gboolean px = stretch_in_pixels (sd);

  sd->busy = TRUE;
  for (int i = 0; i < 2; i++)
    {
      int base = i == 0 ? sd->base_w : sd->base_h;
      GtkAdjustment *adj = adw_spin_row_get_adjustment (ADW_SPIN_ROW (sd->rows[i]));

      gtk_adjustment_configure (adj, px ? round (base * sd->pct[i] / 100.0) : round (sd->pct[i]), 1,
                                px ? MAX (base * 5.0, 2) : 500, 1, 10, 0);
      spin_row_set_unit (sd->rows[i], px ? " px" : "%");
    }
  sd->busy = FALSE;
}

/* With the lock on, switching on the aspect ratio makes both axes follow the first. */
static void
on_stretch_lock_changed (AdwSwitchRow *lock, GParamSpec *pspec, StretchDialog *sd)
{
  if (!adw_switch_row_get_active (lock))
    return;
  sd->busy = TRUE;
  sd->pct[1] = sd->pct[0];
  stretch_show (sd, 1);
  sd->busy = FALSE;
}

static void
on_stretch_response (AdwAlertDialog *dialog, const char *response, StretchDialog *sd)
{
  if (g_str_equal (response, "ok"))
    easel_canvas_stretch_skew (sd->canvas, sd->pct[0], sd->pct[1],
                               adw_spin_row_get_value (ADW_SPIN_ROW (sd->skew[0])),
                               adw_spin_row_get_value (ADW_SPIN_ROW (sd->skew[1])));
  g_free (sd);
}

void
easel_dialog_stretch_skew (GtkWidget *parent, EaselCanvas *canvas)
{
  StretchDialog *sd = g_new0 (StretchDialog, 1);
  GtkWidget *stretch_group = adw_preferences_group_new ();
  GtkWidget *skew_group = adw_preferences_group_new ();
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  AdwDialog *dialog = adw_alert_dialog_new (_("Stretch and Skew"), NULL);
  const char *const units[] = { _("Percent"), _("Pixels"), NULL };

  sd->canvas = canvas;
  sd->pct[0] = sd->pct[1] = 100;
  easel_canvas_get_transform_size (canvas, &sd->base_w, &sd->base_h);
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (stretch_group), _("Stretch"));
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (skew_group), _("Skew"));
  gtk_box_append (GTK_BOX (content), stretch_group);
  gtk_box_append (GTK_BOX (content), skew_group);

  sd->by = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (sd->by), _("Stretch by"));
  adw_combo_row_set_model (ADW_COMBO_ROW (sd->by), G_LIST_MODEL (gtk_string_list_new (units)));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stretch_group), sd->by);

  sd->rows[0] = unit_spin_row (_("Horizontally"), 1, 500, 100, "%");
  sd->rows[1] = unit_spin_row (_("Vertically"), 1, 500, 100, "%");
  sd->lock = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (sd->lock), _("Keep aspect ratio"));
  adw_switch_row_set_active (ADW_SWITCH_ROW (sd->lock), TRUE);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stretch_group), sd->rows[0]);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stretch_group), sd->rows[1]);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (stretch_group), sd->lock);

  sd->skew[0] = unit_spin_row (_("Horizontally"), -89, 89, 0, "°");
  sd->skew[1] = unit_spin_row (_("Vertically"), -89, 89, 0, "°");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (skew_group), sd->skew[0]);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (skew_group), sd->skew[1]);

  g_signal_connect (sd->rows[0], "notify::value", G_CALLBACK (on_stretch_value_changed), sd);
  g_signal_connect (sd->rows[1], "notify::value", G_CALLBACK (on_stretch_value_changed), sd);
  g_signal_connect (sd->by, "notify::selected", G_CALLBACK (on_stretch_unit_changed), sd);
  g_signal_connect (sd->lock, "notify::active", G_CALLBACK (on_stretch_lock_changed), sd);

  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), content);
  easel_dialog_add_ok_cancel (ADW_ALERT_DIALOG (dialog));
  g_signal_connect (dialog, "response", G_CALLBACK (on_stretch_response), sd);
  adw_dialog_present (dialog, parent);
}

typedef struct
{
  EaselCanvas *canvas;
  GtkWidget   *show, *w, *h, *axo, *axo_w;
} GridDialog;

static void
on_grid_response (AdwAlertDialog *dialog, const char *response, GridDialog *gd)
{
  if (g_str_equal (response, "ok"))
    easel_canvas_set_grid (gd->canvas, adw_switch_row_get_active (ADW_SWITCH_ROW (gd->show)),
                           (int) adw_spin_row_get_value (ADW_SPIN_ROW (gd->w)),
                           (int) adw_spin_row_get_value (ADW_SPIN_ROW (gd->h)),
                           adw_switch_row_get_active (ADW_SWITCH_ROW (gd->axo)),
                           (int) adw_spin_row_get_value (ADW_SPIN_ROW (gd->axo_w)));
  g_free (gd);
}

void
easel_dialog_grid (GtkWidget *parent, EaselCanvas *canvas)
{
  GridDialog *gd = g_new0 (GridDialog, 1);
  AdwDialog *dialog = adw_alert_dialog_new (_("Grid"), NULL);
  GtkWidget *group = adw_preferences_group_new ();
  gboolean show, axo;
  int gw, gh, aw;

  gd->canvas = canvas;
  easel_canvas_get_grid (canvas, &show, &gw, &gh, &axo, &aw);

  gd->show = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (gd->show), _("Show grid"));
  adw_switch_row_set_active (ADW_SWITCH_ROW (gd->show), show);
  gd->w = unit_spin_row (_("Width"), 1, 4096, gw, " px");
  gd->h = unit_spin_row (_("Height"), 1, 4096, gh, " px");
  gd->axo = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (gd->axo), _("Show axonometric grid"));
  adw_switch_row_set_active (ADW_SWITCH_ROW (gd->axo), axo);
  gd->axo_w = unit_spin_row (_("Width"), 1, 4096, aw, " px");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), gd->show);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), gd->w);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), gd->h);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), gd->axo);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), gd->axo_w);

  g_object_bind_property (gd->show, "active", gd->w, "sensitive", G_BINDING_SYNC_CREATE);
  g_object_bind_property (gd->show, "active", gd->h, "sensitive", G_BINDING_SYNC_CREATE);
  g_object_bind_property (gd->show, "active", gd->axo, "sensitive", G_BINDING_SYNC_CREATE);
  g_object_bind_property (gd->axo, "active", gd->axo_w, "sensitive", G_BINDING_SYNC_CREATE);

  adw_dialog_set_content_width (dialog, 440);
  adw_alert_dialog_set_body (ADW_ALERT_DIALOG (dialog),
                             _("Lines are drawn only when cells are at least 4 screen pixels wide, so zoom in to see a fine grid."));
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), group);
  easel_dialog_add_ok_cancel (ADW_ALERT_DIALOG (dialog));
  g_signal_connect (dialog, "response", G_CALLBACK (on_grid_response), gd);
  adw_dialog_present (dialog, parent);
}
