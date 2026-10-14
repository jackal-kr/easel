/* Easel's own font picker for the Text tool: family list with search, size and
 * the four character styles in one window, with a live preview.
 *
 * This replaces GtkFontDialog, whose list could come up blank inside Flatpak
 * (see git history of toolbox.c). The families come straight from Pango's font
 * map, so nothing here depends on how GTK's chooser loads its list. */
#include "ui/fontdialog.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <pango/pangocairo.h>

#include "ui/tool.h"

typedef struct
{
  EaselFontDone done;
  gpointer      data;
  AdwDialog    *dialog;
  GtkListView  *list;
  GtkSingleSelection *selection;
  GtkStringFilter *filter;
  GtkWidget    *size;
  GtkWidget    *style[4]; /* bold, italic, underline, strike */
  GtkWidget    *preview;  /* GtkDrawingArea */
  GtkWidget    *color;    /* GtkColorDialogButton */
  uint32_t      first_argb;
} PickerState;

static const guint style_flags[4] = { EASEL_TEXT_BOLD, EASEL_TEXT_ITALIC, EASEL_TEXT_UNDERLINE, EASEL_TEXT_STRIKE };

static int
compare_families (gconstpointer a, gconstpointer b)
{
  return g_utf8_collate (*(const char **) a, *(const char **) b);
}

/* Sorted names of every font family Pango can see. */
static GtkStringList *
list_families (void)
{
  PangoFontFamily **families;
  int n;
  GPtrArray *names = g_ptr_array_new ();
  GtkStringList *model = gtk_string_list_new (NULL);

  pango_font_map_list_families (pango_cairo_font_map_get_default (), &families, &n);
  for (int i = 0; i < n; i++)
    g_ptr_array_add (names, (gpointer) pango_font_family_get_name (families[i]));
  g_ptr_array_sort (names, compare_families);
  for (guint i = 0; i < names->len; i++)
    gtk_string_list_append (model, g_ptr_array_index (names, i));
  g_ptr_array_free (names, TRUE);
  g_free (families);
  return model;
}

static const char *
selected_family (PickerState *st)
{
  GtkStringObject *item = gtk_single_selection_get_selected_item (st->selection);

  return item != NULL ? gtk_string_object_get_string (item) : NULL;
}

/* Draws the sample at the chosen size, or smaller if it would not fit: the
 * text is scaled down until all of it is inside the preview area. */
static void
draw_preview (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  PickerState *st = data;
  const char *family = selected_family (st);
  double wanted = gtk_spin_button_get_value (GTK_SPIN_BUTTON (st->size));
  const int pad = 12;
  GdkRGBA color = *gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (st->color));
  PangoLayout *layout = pango_cairo_create_layout (cr);
  double size = wanted;

  /* White paper, like the picture the text will land on. */
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_paint (cr);
  pango_layout_set_text (layout, _("The sysadmin quickly sudoed the gnomes before they compiled the last of the coffee. "
       "Nobody blamed the cat, because the cat had already pushed to production on a Friday. "
       "The penguins, meanwhile, held a very serious meeting about kernel panics and herring, "
       "and declared it a feature."), -1);
  pango_layout_set_width (layout, (width - 2 * pad) * PANGO_SCALE);
  pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);

  for (;;)
    {
      PangoFontDescription *desc = pango_font_description_new ();
      PangoAttrList *attrs = pango_attr_list_new ();
      int h;

      if (family != NULL)
        pango_font_description_set_family (desc, family);
      pango_font_description_set_size (desc, (int) (size * PANGO_SCALE));
      if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (st->style[0])))
        pango_font_description_set_weight (desc, PANGO_WEIGHT_BOLD);
      if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (st->style[1])))
        pango_font_description_set_style (desc, PANGO_STYLE_ITALIC);
      pango_layout_set_font_description (layout, desc);
      if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (st->style[2])))
        pango_attr_list_insert (attrs, pango_attr_underline_new (PANGO_UNDERLINE_SINGLE));
      if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (st->style[3])))
        pango_attr_list_insert (attrs, pango_attr_strikethrough_new (TRUE));
      pango_layout_set_attributes (layout, attrs);
      pango_layout_get_pixel_size (layout, NULL, &h);
      pango_attr_list_unref (attrs);
      pango_font_description_free (desc);
      if (h <= height - 2 * pad || size <= 4)
        break;
      size = MAX (4, size * 0.92);
    }

  cairo_move_to (cr, pad, pad);
  gdk_cairo_set_source_rgba (cr, &color);
  pango_cairo_show_layout (cr, layout);
  g_object_unref (layout);
}

static void
update_preview (PickerState *st)
{
  gtk_widget_queue_draw (st->preview);
}

static void
on_value_changed (gpointer widget, PickerState *st)
{
  update_preview (st);
}

static void
on_selected_changed (GObject *selection, GParamSpec *pspec, PickerState *st)
{
  update_preview (st);
}

/* The search box swallows Esc to clear itself, so catch it first and close. */
static gboolean
on_dialog_key (GtkEventControllerKey *ctrl, guint keyval, guint keycode, GdkModifierType state, gpointer data)
{
  PickerState *st = data;

  if (keyval != GDK_KEY_Escape)
    return GDK_EVENT_PROPAGATE;
  adw_dialog_close (st->dialog);
  return GDK_EVENT_STOP;
}

static void
on_search_changed (GtkSearchEntry *entry, PickerState *st)
{
  gtk_string_filter_set_search (st->filter, gtk_editable_get_text (GTK_EDITABLE (entry)));
}

static void
on_select (GtkButton *button, PickerState *st)
{
  const char *family = selected_family (st);

  if (family != NULL)
    {
      PangoFontDescription *desc = pango_font_description_new ();
      char *font;
      guint flags = 0;

      pango_font_description_set_family (desc, family);
      pango_font_description_set_size (desc, (int) (gtk_spin_button_get_value (GTK_SPIN_BUTTON (st->size)) * PANGO_SCALE));
      font = pango_font_description_to_string (desc);
      for (int i = 0; i < 4; i++)
        if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (st->style[i])))
          flags |= style_flags[i];
      const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (st->color));
      uint32_t argb = 0xFF000000u | ((uint32_t) (rgba->red * 255 + 0.5) << 16) |
                      ((uint32_t) (rgba->green * 255 + 0.5) << 8) | (uint32_t) (rgba->blue * 255 + 0.5);

      st->done (font, flags, argb, (argb & 0xFFFFFF) != (st->first_argb & 0xFFFFFF), st->data);
      g_free (font);
      pango_font_description_free (desc);
    }
  adw_dialog_close (st->dialog);
}

static void
on_cancel (GtkButton *button, PickerState *st)
{
  adw_dialog_close (st->dialog);
}

/* Each row is written in its own font, so the list doubles as a specimen sheet. */
static void
setup_row (GtkSignalListItemFactory *factory, GtkListItem *item)
{
  GtkWidget *label = gtk_label_new (NULL);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_set_margin_top (label, 6);
  gtk_widget_set_margin_bottom (label, 6);
  gtk_widget_set_margin_start (label, 8);
  gtk_list_item_set_child (item, label);
}

static void
bind_row (GtkSignalListItemFactory *factory, GtkListItem *item)
{
  GtkWidget *label = gtk_list_item_get_child (item);
  const char *family = gtk_string_object_get_string (gtk_list_item_get_item (item));
  PangoAttrList *attrs = pango_attr_list_new ();
  PangoFontDescription *desc = pango_font_description_new ();

  pango_font_description_set_family (desc, family);
  pango_attr_list_insert (attrs, pango_attr_font_desc_new (desc));
  gtk_label_set_text (GTK_LABEL (label), family);
  gtk_label_set_attributes (GTK_LABEL (label), attrs);
  pango_attr_list_unref (attrs);
  pango_font_description_free (desc);
}

/* Moves the selection (and the scroll position) to `family`, if it exists. */
static void
select_family (PickerState *st, const char *family)
{
  GListModel *model = G_LIST_MODEL (st->selection);

  for (guint i = 0; i < g_list_model_get_n_items (model); i++)
    {
      g_autoptr (GtkStringObject) item = g_list_model_get_item (model, i);

      if (g_ascii_strcasecmp (gtk_string_object_get_string (item), family) == 0)
        {
          gtk_single_selection_set_selected (st->selection, i);
          gtk_list_view_scroll_to (st->list, i, GTK_LIST_SCROLL_SELECT, NULL);
          return;
        }
    }
}

void
easel_font_dialog_present (GtkWidget *parent, const char *font, guint flags, uint32_t argb,
                           EaselFontDone done, gpointer data)
{
  PickerState *st = g_new0 (PickerState, 1);
  PangoFontDescription *current = pango_font_description_from_string (font);
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *cancel = gtk_button_new_with_mnemonic (_("_Cancel"));
  GtkWidget *select = gtk_button_new_with_label (_("Select"));
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *search = gtk_search_entry_new ();
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *options = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *size_label = gtk_label_new (_("Size"));
  static const char *style_names[4] = { N_("Bold"), N_("Italic"), N_("Underline"), N_("Strikethrough") };
  static const char *style_icons[4] = { "format-text-bold-symbolic", "format-text-italic-symbolic",
                                        "format-text-underline-symbolic", "format-text-strikethrough-symbolic" };

  st->done = done;
  st->first_argb = argb;
  st->data = data;
  st->dialog = adw_dialog_new ();
  adw_dialog_set_title (st->dialog, _("Font"));
  adw_dialog_set_content_width (st->dialog, 760);
  adw_dialog_set_content_height (st->dialog, 480);

  /* List: all families -> filtered by the search box -> single selection. */
  GtkStringList *families = list_families ();
  st->filter = gtk_string_filter_new (gtk_property_expression_new (GTK_TYPE_STRING_OBJECT, NULL, "string"));
  gtk_string_filter_set_ignore_case (st->filter, TRUE);
  GtkFilterListModel *filtered = gtk_filter_list_model_new (G_LIST_MODEL (families), GTK_FILTER (st->filter));
  st->selection = gtk_single_selection_new (G_LIST_MODEL (filtered));
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (setup_row), NULL);
  g_signal_connect (factory, "bind", G_CALLBACK (bind_row), NULL);
  st->list = GTK_LIST_VIEW (gtk_list_view_new (GTK_SELECTION_MODEL (g_object_ref (st->selection)), factory));
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), GTK_WIDGET (st->list));
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_widget_add_css_class (scroller, "card");

  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (search), _("Search font name"));
  g_signal_connect (search, "search-changed", G_CALLBACK (on_search_changed), st);

  /* Size and styles. */
  double size = pango_font_description_get_size (current) > 0
                    ? pango_font_description_get_size (current) / (double) PANGO_SCALE
                    : 11;
  st->size = gtk_spin_button_new_with_range (1, 200, 1);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (st->size), size);
  g_signal_connect (st->size, "value-changed", G_CALLBACK (on_value_changed), st);
  gtk_widget_set_valign (size_label, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (options), size_label);
  gtk_box_append (GTK_BOX (options), st->size);

  /* Text colour. */
  GtkColorDialog *color_dialog = gtk_color_dialog_new ();
  GdkRGBA initial = { ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f, 1 };
  gtk_color_dialog_set_with_alpha (color_dialog, FALSE);
  gtk_color_dialog_set_title (color_dialog, _("Text Color"));
  st->color = gtk_color_dialog_button_new (color_dialog);
  gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (st->color), &initial);
  gtk_widget_set_tooltip_text (st->color, _("Text Color"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (st->color), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Text Color"), -1);
  g_signal_connect (st->color, "notify::rgba", G_CALLBACK (on_selected_changed), st);
  gtk_box_append (GTK_BOX (options), st->color);

  /* Bold, italic, underline and strikethrough as one joined row of toggle buttons. */
  GtkWidget *styles = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (styles, "linked");
  gtk_widget_set_hexpand (styles, TRUE);
  gtk_widget_set_halign (styles, GTK_ALIGN_END);
  for (int i = 0; i < 4; i++)
    {
      st->style[i] = gtk_toggle_button_new ();
      gtk_button_set_icon_name (GTK_BUTTON (st->style[i]), style_icons[i]);
      gtk_widget_set_tooltip_text (st->style[i], _(style_names[i]));
      gtk_accessible_update_property (GTK_ACCESSIBLE (st->style[i]), GTK_ACCESSIBLE_PROPERTY_LABEL, _(style_names[i]), -1);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (st->style[i]), (flags & style_flags[i]) != 0);
      g_signal_connect (st->style[i], "toggled", G_CALLBACK (on_value_changed), st);
      gtk_box_append (GTK_BOX (styles), st->style[i]);
    }
  gtk_box_append (GTK_BOX (options), styles);

  st->preview = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (st->preview), draw_preview, st, NULL);
  gtk_widget_set_hexpand (st->preview, TRUE);
  gtk_widget_set_vexpand (st->preview, TRUE);
  gtk_widget_set_overflow (st->preview, GTK_OVERFLOW_HIDDEN);
  gtk_widget_add_css_class (st->preview, "card");

  /* Left: search and family list. Right: preview over the size and styles. */
  GtkWidget *left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_size_request (left, 260, -1);
  gtk_box_append (GTK_BOX (left), search);
  gtk_box_append (GTK_BOX (left), scroller);
  gtk_box_set_homogeneous (GTK_BOX (box), FALSE);
  gtk_orientable_set_orientation (GTK_ORIENTABLE (box), GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing (GTK_BOX (box), 12);
  gtk_box_append (GTK_BOX (right), st->preview);
  gtk_box_append (GTK_BOX (right), options);
  gtk_box_append (GTK_BOX (box), left);
  gtk_box_append (GTK_BOX (box), right);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);
  gtk_widget_set_margin_bottom (box, 12);

  gtk_widget_add_css_class (select, "suggested-action");
  g_signal_connect (cancel, "clicked", G_CALLBACK (on_cancel), st);
  g_signal_connect (select, "clicked", G_CALLBACK (on_select), st);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), select);
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), box);
  adw_dialog_set_child (st->dialog, toolbar);
  adw_dialog_set_default_widget (st->dialog, select);
  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_dialog_key), st);
  gtk_widget_add_controller (GTK_WIDGET (st->dialog), keys);
  adw_dialog_set_focus (st->dialog, search);
  g_signal_connect_swapped (st->dialog, "closed", G_CALLBACK (g_free), st);

  g_signal_connect (st->selection, "notify::selected", G_CALLBACK (on_selected_changed), st);
  if (pango_font_description_get_family (current) != NULL)
    select_family (st, pango_font_description_get_family (current));
  update_preview (st);
  pango_font_description_free (current);

  adw_dialog_present (st->dialog, parent);
}
