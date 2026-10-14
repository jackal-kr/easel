#include "ui/colorbox.h"
#include <glib/gi18n.h>

#include <string.h>

#include "core/document.h"
#include "ui/config.h"
#include "ui/settings.h"

#define PALETTE_COLUMNS 14
#define PALETTE_ROWS    2
#define CELL            20
#define GAP             2
#define STEP            (CELL + GAP)

#define SWATCH_W 46
#define SWATCH_H 42

/* The classic 28-colour Paint palette, top row first. */
static const uint32_t palette[PALETTE_COLUMNS * PALETTE_ROWS] = {
  EASEL_RGB (0, 0, 0),       EASEL_RGB (128, 128, 128), EASEL_RGB (128, 0, 0),
  EASEL_RGB (128, 128, 0),   EASEL_RGB (0, 128, 0),     EASEL_RGB (0, 128, 128),
  EASEL_RGB (0, 0, 128),     EASEL_RGB (128, 0, 128),   EASEL_RGB (128, 128, 64),
  EASEL_RGB (0, 64, 64),     EASEL_RGB (0, 128, 255),   EASEL_RGB (0, 64, 128),
  EASEL_RGB (64, 0, 255),    EASEL_RGB (128, 64, 0),

  EASEL_RGB (255, 255, 255), EASEL_RGB (192, 192, 192), EASEL_RGB (255, 0, 0),
  EASEL_RGB (255, 255, 0),   EASEL_RGB (0, 255, 0),     EASEL_RGB (0, 255, 255),
  EASEL_RGB (0, 0, 255),     EASEL_RGB (255, 0, 255),   EASEL_RGB (255, 255, 128),
  EASEL_RGB (0, 255, 128),   EASEL_RGB (128, 255, 255), EASEL_RGB (128, 128, 255),
  EASEL_RGB (255, 0, 128),   EASEL_RGB (255, 128, 64),
};

/* Names of the classic colours, same order as `palette`, for screen readers. */
static const char *const palette_names[PALETTE_COLUMNS * PALETTE_ROWS] = {
  N_("Black"), N_("Dark Gray"), N_("Dark Red"), N_("Olive"), N_("Dark Green"), N_("Teal"), N_("Dark Blue"),
  N_("Purple"), N_("Dark Khaki"), N_("Dark Teal"), N_("Azure"), N_("Navy Blue"), N_("Violet"), N_("Brown"),
  N_("White"), N_("Light Gray"), N_("Red"), N_("Yellow"), N_("Lime"), N_("Cyan"), N_("Blue"), N_("Magenta"),
  N_("Light Yellow"), N_("Spring Green"), N_("Light Cyan"), N_("Periwinkle"), N_("Rose"), N_("Orange"),
};

/* "Red" for a palette color, "Red (#F00001)" for the nearest name of any other. */
static char *
describe_color (uint32_t c)
{
  int best = 0;
  long best_d = -1;

  for (int i = 0; i < PALETTE_COLUMNS * PALETTE_ROWS; i++)
    {
      long dr = (long) ((c >> 16) & 0xFF) - (long) ((palette[i] >> 16) & 0xFF);
      long dg = (long) ((c >> 8) & 0xFF) - (long) ((palette[i] >> 8) & 0xFF);
      long db = (long) (c & 0xFF) - (long) (palette[i] & 0xFF);
      long d = dr * dr + dg * dg + db * db;

      if (best_d < 0 || d < best_d)
        {
          best_d = d;
          best = i;
        }
    }
  if (best_d == 0)
    return g_strdup (_(palette_names[best]));
  return g_strdup_printf (_("%s (#%06X)"), _(palette_names[best]), c & 0xFFFFFFu);
}

/* The two overlapping squares at the left: foreground on top of background,
 * as in the original. Positions in pixels inside the swatch widget. */
static const GdkRectangle fg_rect = { 2, 2, 28, 28 };
static const GdkRectangle bg_rect = { 16, 12, 28, 28 };

struct _EaselColorBox
{
  GtkBox parent_instance;

  uint32_t foreground;
  uint32_t background;
  uint32_t colors[PALETTE_COLUMNS * PALETTE_ROWS]; /* the palette, editable */
  int      selected;                               /* last palette cell clicked, -1 none */

  GtkWidget *swatch_area;
  GtkWidget *palette_area;
};

G_DEFINE_FINAL_TYPE (EaselColorBox, easel_color_box, GTK_TYPE_BOX)

enum { SIGNAL_COLORS_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* Keeps what a screen reader says about the swatches and the palette current. */
static void
update_a11y (EaselColorBox *self)
{
  g_autofree char *fg = describe_color (self->foreground);
  g_autofree char *bg = describe_color (self->background);
  g_autofree char *swatch = g_strdup_printf (_("Foreground: %s. Background: %s."), fg, bg);
  int cell = self->selected < 0 ? 0 : self->selected;
  g_autofree char *name = describe_color (self->colors[cell]);
  g_autofree char *cell_text = g_strdup_printf (_("Color %d of %d: %s. Foreground: %s. Background: %s."),
                                               cell + 1, PALETTE_COLUMNS * PALETTE_ROWS, name, fg, bg);

  gtk_accessible_update_property (GTK_ACCESSIBLE (self->swatch_area), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Foreground and background colors"), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                  swatch, -1);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->palette_area), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Color palette"), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, cell_text, -1);
}

static void
announce (EaselColorBox *self, const char *text)
{
  gtk_accessible_announce (GTK_ACCESSIBLE (self->palette_area), text, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
}

static void
set_source_argb (cairo_t *cr, uint32_t c)
{
  cairo_set_source_rgb (cr, ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0);
}


/* The palette is saved in settings as a list of 28 "#rrggbb" strings. An empty
 * list means the user never edited it, so use the classic colours. */
static void
load_palette (EaselColorBox *self)
{
  GSettings *s = easel_settings ();
  g_auto (GStrv) hex = s != NULL ? g_settings_get_strv (s, "palette") : NULL;

  memcpy (self->colors, palette, sizeof palette);
  if (hex == NULL || g_strv_length (hex) != PALETTE_COLUMNS * PALETTE_ROWS)
    return;
  for (int i = 0; i < PALETTE_COLUMNS * PALETTE_ROWS; i++)
    {
      uint32_t argb;
      if (easel_parse_hex_color (hex[i], &argb))
        self->colors[i] = argb;
    }
}

static void
save_palette (EaselColorBox *self)
{
  GSettings *s = easel_settings ();
  if (s == NULL)
    return;

  GStrvBuilder *builder = g_strv_builder_new ();
  for (int i = 0; i < PALETTE_COLUMNS * PALETTE_ROWS; i++)
    {
      g_autofree char *hex = easel_argb_to_hex (self->colors[i]);
      g_strv_builder_add (builder, hex);
    }
  g_auto (GStrv) strv = g_strv_builder_end (builder);
  g_strv_builder_unref (builder);
  g_settings_set_strv (s, "palette", (const char *const *) strv);
}

static void
on_palette_setting_changed (GSettings *settings, const char *key, EaselColorBox *self)
{
  load_palette (self);
  gtk_widget_queue_draw (self->palette_area);
}

static void
colors_changed (EaselColorBox *self)
{
  update_a11y (self);
  gtk_widget_queue_draw (self->swatch_area);
  g_signal_emit (self, signals[SIGNAL_COLORS_CHANGED], 0);
}

static void
draw_framed_square (cairo_t *cr, GtkWidget *widget, const GdkRectangle *r, uint32_t color)
{
  GdkRGBA border;
  gtk_widget_get_color (widget, &border);
  border.alpha *= 0.5;

  set_source_argb (cr, color);
  cairo_rectangle (cr, r->x, r->y, r->width, r->height);
  cairo_fill (cr);

  gdk_cairo_set_source_rgba (cr, &border);
  cairo_set_line_width (cr, 1);
  cairo_rectangle (cr, r->x + 0.5, r->y + 0.5, r->width - 1, r->height - 1);
  cairo_stroke (cr);
}

static void
draw_swatch (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  EaselColorBox *self = data;
  draw_framed_square (cr, GTK_WIDGET (area), &bg_rect, self->background);
  draw_framed_square (cr, GTK_WIDGET (area), &fg_rect, self->foreground);
}

static void
draw_palette (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  for (int i = 0; i < PALETTE_COLUMNS * PALETTE_ROWS; i++)
    {
      GdkRectangle r = { (i % PALETTE_COLUMNS) * STEP, (i / PALETTE_COLUMNS) * STEP, CELL, CELL };
      draw_framed_square (cr, GTK_WIDGET (area), &r, ((EaselColorBox *) data)->colors[i]);
    }

  /* Keyboard focus ring on the current cell. */
  EaselColorBox *self = data;
  if (gtk_widget_has_focus (GTK_WIDGET (area)) && self->selected >= 0)
    {
      GdkRGBA c;
      gtk_widget_get_color (GTK_WIDGET (area), &c);
      gdk_cairo_set_source_rgba (cr, &c);
      cairo_set_line_width (cr, 2);
      cairo_rectangle (cr, (self->selected % PALETTE_COLUMNS) * STEP - 1,
                       (self->selected / PALETTE_COLUMNS) * STEP - 1, CELL + 2, CELL + 2);
      cairo_stroke (cr);
    }
}

typedef struct
{
  EaselColorBox *box; /* strong reference */
  gboolean       foreground;
  int            cell;       /* palette cell being edited, or -1 */
} ChooseData;

static void
on_color_chosen (GObject *source, GAsyncResult *result, gpointer user_data)
{
  ChooseData *data = user_data;
  g_autoptr (GError) error = NULL;
  GdkRGBA *rgba = gtk_color_dialog_choose_rgba_finish (GTK_COLOR_DIALOG (source), result, &error);

  /* A dismissed dialog is not an error worth reporting. */
  if (rgba != NULL)
    {
      if (data->cell >= 0)
        {
          /* Edit Colors: the cell takes the new colour, and so does the pen. */
          data->box->colors[data->cell] = easel_rgba_to_argb (rgba);
          save_palette (data->box);
          gtk_widget_queue_draw (data->box->palette_area);
        }
      if (data->foreground)
        easel_color_box_set_foreground (data->box, easel_rgba_to_argb (rgba));
      else
        easel_color_box_set_background (data->box, easel_rgba_to_argb (rgba));
      gdk_rgba_free (rgba);
    }

  g_object_unref (data->box);
  g_free (data);
}

static void
choose_color (EaselColorBox *self, gboolean foreground, int cell)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  GdkRGBA initial = easel_argb_to_rgba (cell >= 0 ? self->colors[cell] : foreground ? self->foreground : self->background, 1);
  g_autoptr (GtkColorDialog) dialog = gtk_color_dialog_new ();
  ChooseData *data = g_new0 (ChooseData, 1);

  data->box = g_object_ref (self);
  data->foreground = foreground;
  data->cell = cell;

  gtk_color_dialog_set_title (dialog, cell >= 0 ? _("Edit Color") : foreground ? _("Foreground Color") : _("Background Color"));
  gtk_color_dialog_set_with_alpha (dialog, FALSE);
  gtk_color_dialog_choose_rgba (dialog, GTK_WINDOW (root), &initial, NULL, on_color_chosen, data);
}

static gboolean
rect_contains (const GdkRectangle *r, double x, double y)
{
  return x >= r->x && x < r->x + r->width && y >= r->y && y < r->y + r->height;
}

static void
on_swatch_pressed (GtkGestureClick *gesture, int n_press, double x, double y, EaselColorBox *self)
{
  if (rect_contains (&fg_rect, x, y))
    choose_color (self, TRUE, -1);
  else if (rect_contains (&bg_rect, x, y))
    choose_color (self, FALSE, -1);
}

/* The focus ring is only useful while navigating with the keyboard. After a
 * colour has been picked it is hidden so it doesn't sit on an old cell. */
static void
palette_done (EaselColorBox *self)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));

  if (gtk_widget_has_focus (self->palette_area) && GTK_IS_WINDOW (root))
    gtk_window_set_focus (GTK_WINDOW (root), NULL);
  gtk_widget_queue_draw (self->palette_area);
}

static void
on_palette_pressed (GtkGestureClick *gesture, int n_press, double x, double y, EaselColorBox *self)
{
  guint button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
  int col = (int) x / STEP;
  int row = (int) y / STEP;

  /* Ignore clicks in the gaps between cells. */
  if (x < 0 || y < 0 || (int) x % STEP >= CELL || (int) y % STEP >= CELL
      || col >= PALETTE_COLUMNS || row >= PALETTE_ROWS)
    return;

  int cell = row * PALETTE_COLUMNS + col;
  uint32_t color = self->colors[cell];

  self->selected = cell;
  if (n_press == 2 && button == GDK_BUTTON_PRIMARY)
    {
      choose_color (self, TRUE, cell); /* double-click edits the colour */
      return;
    }
  g_autofree char *name = describe_color (color);
  g_autofree char *said = NULL;

  if (button == GDK_BUTTON_PRIMARY)
    {
      easel_color_box_set_foreground (self, color);
      said = g_strdup_printf (_("Foreground color: %s"), name);
    }
  else if (button == GDK_BUTTON_SECONDARY)
    {
      easel_color_box_set_background (self, color);
      said = g_strdup_printf (_("Background color: %s"), name);
    }
  if (said != NULL)
    announce (self, said);
  palette_done (self);
}

/* Arrow keys move over the palette; Enter/Space picks the foreground,
 * Shift+Enter/Shift+Space (or Menu) the background, F2 edits the cell. */
static gboolean
on_palette_key (GtkEventControllerKey *controller, guint keyval, guint keycode,
                GdkModifierType state, EaselColorBox *self)
{
  int cell = self->selected < 0 ? 0 : self->selected;
  int col = cell % PALETTE_COLUMNS, row = cell / PALETTE_COLUMNS;

  switch (keyval)
    {
    case GDK_KEY_Left:  col = MAX (col - 1, 0); break;
    case GDK_KEY_Right: col = MIN (col + 1, PALETTE_COLUMNS - 1); break;
    case GDK_KEY_Up:    row = MAX (row - 1, 0); break;
    case GDK_KEY_Down:  row = MIN (row + 1, PALETTE_ROWS - 1); break;
    case GDK_KEY_Home:  col = 0; break;
    case GDK_KEY_End:   col = PALETTE_COLUMNS - 1; break;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_space:
      {
        g_autofree char *name = describe_color (self->colors[cell]);
        g_autofree char *said = g_strdup_printf ((state & GDK_SHIFT_MASK) ? _("Background color: %s")
                                                                          : _("Foreground color: %s"), name);

        if (state & GDK_SHIFT_MASK)
          easel_color_box_set_background (self, self->colors[cell]);
        else
          easel_color_box_set_foreground (self, self->colors[cell]);
        self->selected = cell;
        announce (self, said);
      }
      palette_done (self);
      return GDK_EVENT_STOP;
    case GDK_KEY_Menu:
      easel_color_box_set_background (self, self->colors[cell]);
      palette_done (self);
      return GDK_EVENT_STOP;
    case GDK_KEY_F2:
      self->selected = cell;
      choose_color (self, TRUE, cell);
      return GDK_EVENT_STOP;
    default:
      return GDK_EVENT_PROPAGATE;
    }
  self->selected = row * PALETTE_COLUMNS + col;
  update_a11y (self);
  {
    g_autofree char *name = describe_color (self->colors[self->selected]);
    g_autofree char *said = g_strdup_printf (_("Color %d of %d: %s"), self->selected + 1,
                                             PALETTE_COLUMNS * PALETTE_ROWS, name);
    announce (self, said);
  }
  gtk_widget_queue_draw (self->palette_area);
  return GDK_EVENT_STOP;
}

static void
on_palette_focus (GtkEventControllerFocus *controller, EaselColorBox *self)
{
  if (gtk_event_controller_focus_contains_focus (controller) && self->selected < 0)
    self->selected = 0;
  gtk_widget_queue_draw (self->palette_area);
}

static void
on_swap_clicked (GtkButton *button, EaselColorBox *self)
{
  easel_color_box_swap (self);
}

static GtkWidget *
make_drawing_area (int width, int height, GtkDrawingAreaDrawFunc draw, gpointer data)
{
  GtkWidget *area = gtk_drawing_area_new ();
  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (area), width);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (area), height);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (area), draw, data, NULL);
  gtk_widget_set_valign (area, GTK_ALIGN_CENTER);
  return area;
}

static void
easel_color_box_class_init (EaselColorBoxClass *klass)
{
  signals[SIGNAL_COLORS_CHANGED] =
    g_signal_new ("colors-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
easel_color_box_init (EaselColorBox *self)
{
  self->foreground = EASEL_RGB (0, 0, 0);
  self->background = EASEL_RGB (255, 255, 255);
  load_palette (self);
  self->selected = -1;

  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing (GTK_BOX (self), 8);
  gtk_widget_set_margin_start (GTK_WIDGET (self), 8);
  gtk_widget_set_margin_end (GTK_WIDGET (self), 8);
  gtk_widget_set_margin_top (GTK_WIDGET (self), 6);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self), 6);

  self->swatch_area = make_drawing_area (SWATCH_W, SWATCH_H, draw_swatch, self);
  gtk_widget_set_tooltip_text (self->swatch_area, _("Foreground and background colors. Click a square to choose a color."));
  GtkGesture *swatch_click = gtk_gesture_click_new ();
  g_signal_connect (swatch_click, "pressed", G_CALLBACK (on_swatch_pressed), self);
  gtk_widget_add_controller (self->swatch_area, GTK_EVENT_CONTROLLER (swatch_click));
  gtk_box_append (GTK_BOX (self), self->swatch_area);

  GtkWidget *swap = gtk_button_new_with_label ("⇄");
  gtk_widget_add_css_class (swap, "flat");
  gtk_widget_set_valign (swap, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (swap, _("Swap foreground and background (Ctrl+Shift+X)"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (swap), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  _("Swap foreground and background"), -1);
  g_signal_connect (swap, "clicked", G_CALLBACK (on_swap_clicked), self);
  gtk_box_append (GTK_BOX (self), swap);


  self->palette_area = make_drawing_area (PALETTE_COLUMNS * STEP - GAP, PALETTE_ROWS * STEP - GAP,
                                          draw_palette, self);
  gtk_widget_set_tooltip_text (self->palette_area, _("Left-click: foreground. Right-click: background. Double-click: edit this color. Keyboard: Tab to focus, arrows, Enter / Shift+Enter, F2."));
  GtkGesture *palette_click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (palette_click), 0);
  g_signal_connect (palette_click, "pressed", G_CALLBACK (on_palette_pressed), self);
  gtk_widget_add_controller (self->palette_area, GTK_EVENT_CONTROLLER (palette_click));
  if (easel_settings () != NULL)
    g_signal_connect_object (easel_settings (), "changed::palette", G_CALLBACK (on_palette_setting_changed), self, 0);

  gtk_widget_set_focusable (self->palette_area, TRUE);
  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_palette_key), self);
  gtk_widget_add_controller (self->palette_area, keys);
  GtkEventController *focus = gtk_event_controller_focus_new ();
  g_signal_connect (focus, "enter", G_CALLBACK (on_palette_focus), self);
  g_signal_connect (focus, "leave", G_CALLBACK (on_palette_focus), self);
  gtk_widget_add_controller (self->palette_area, focus);
  gtk_box_append (GTK_BOX (self), self->palette_area);
  update_a11y (self);
}

GtkWidget *
easel_color_box_new (void)
{
  return g_object_new (EASEL_TYPE_COLOR_BOX, NULL);
}

uint32_t
easel_color_box_get_foreground (EaselColorBox *self)
{
  g_return_val_if_fail (EASEL_IS_COLOR_BOX (self), 0);
  return self->foreground;
}

uint32_t
easel_color_box_get_background (EaselColorBox *self)
{
  g_return_val_if_fail (EASEL_IS_COLOR_BOX (self), 0);
  return self->background;
}

void
easel_color_box_set_foreground (EaselColorBox *self, uint32_t argb)
{
  g_return_if_fail (EASEL_IS_COLOR_BOX (self));
  if (self->foreground == argb)
    return;
  self->foreground = argb;
  colors_changed (self);
}

void
easel_color_box_set_background (EaselColorBox *self, uint32_t argb)
{
  g_return_if_fail (EASEL_IS_COLOR_BOX (self));
  if (self->background == argb)
    return;
  self->background = argb;
  colors_changed (self);
}

void
easel_color_box_swap (EaselColorBox *self)
{
  g_return_if_fail (EASEL_IS_COLOR_BOX (self));
  uint32_t tmp = self->foreground;
  self->foreground = self->background;
  self->background = tmp;
  colors_changed (self);
}

/* Colors > Edit Colors…: edits the palette cell last clicked; with none,
 * the foreground colour itself. */
void
easel_color_box_edit_colors (EaselColorBox *self)
{
  g_return_if_fail (EASEL_IS_COLOR_BOX (self));
  choose_color (self, TRUE, self->selected);
}
