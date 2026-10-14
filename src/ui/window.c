#include "ui/window.h"

#include "core/document.h"
#include "ui/canvas.h"
#include "ui/colorbox.h"
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
  easel_document_fill (easel_canvas_get_document (self->canvas),
                       easel_color_box_get_background (self->colorbox));
  easel_canvas_invalidate (self->canvas);
}

static const GActionEntry window_actions[] = {
  { "swap-colors", action_swap_colors },
  { "clear-image", action_clear_image },
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
  GtkWidget *size_label = gtk_label_new (size_text);
  gtk_widget_add_css_class (size_label, "caption");
  gtk_widget_add_css_class (size_label, "numeric");
  gtk_widget_set_tooltip_text (size_label, "Image size");
  gtk_box_append (GTK_BOX (bar), size_label);

  return bar;
}

static void
easel_window_class_init (EaselWindowClass *klass)
{
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

  on_colors_changed (self->colorbox, self);
  sync_canvas_tool (self);
}

EaselWindow *
easel_window_new (GtkApplication *app)
{
  return g_object_new (EASEL_TYPE_WINDOW, "application", app, NULL);
}
