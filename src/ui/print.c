#include "ui/print.h"

#include <adwaita.h>

#include "ui/config.h"

static GtkPrintSettings *print_settings;
static GtkPageSetup *page_setup;

static void
on_draw_page (GtkPrintOperation *op, GtkPrintContext *context, int page_nr, EaselCanvas *canvas)
{
  EaselDocument *doc = easel_canvas_get_document (canvas);
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
on_print_done (GtkPrintOperation *op, GtkPrintOperationResult result, EaselCanvas *canvas)
{
  if (result == GTK_PRINT_OPERATION_RESULT_APPLY)
    {
      g_clear_object (&print_settings);
      print_settings = g_object_ref (gtk_print_operation_get_print_settings (op));
    }
  g_object_unref (op);
}

void
easel_print_picture (GtkWindow *parent, EaselCanvas *canvas)
{
  GtkPrintOperation *op = gtk_print_operation_new ();
  g_autoptr (GError) error = NULL;

  easel_canvas_flush (canvas);
  gtk_print_operation_set_n_pages (op, 1);
  gtk_print_operation_set_unit (op, GTK_UNIT_POINTS);
  if (print_settings != NULL)
    gtk_print_operation_set_print_settings (op, print_settings);
  if (page_setup != NULL)
    gtk_print_operation_set_default_page_setup (op, page_setup);
  g_signal_connect (op, "draw-page", G_CALLBACK (on_draw_page), canvas);
  g_signal_connect (op, "done", G_CALLBACK (on_print_done), canvas);
  gtk_print_operation_run (op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, parent, &error);
  if (error != NULL)
    {
      /* "done" releases op. */
      AdwDialog *dialog = adw_alert_dialog_new ("Could not print the picture", error->message);

      adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "ok", "_OK");
      adw_dialog_present (dialog, GTK_WIDGET (parent));
    }
}

