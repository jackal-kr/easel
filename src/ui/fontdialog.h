/* Font picker for the Text tool (see fontdialog.c). */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Called when the user presses Select: `font` is a Pango description with
 * family and size, `flags` the chosen EASEL_TEXT_* character styles, `argb` the
 * text colour (`color_changed` tells whether it differs from what was passed in). */
typedef void (*EaselFontDone) (const char *font, guint flags, uint32_t argb, gboolean color_changed, gpointer data);

void easel_font_dialog_present (GtkWidget *parent, const char *font, guint flags, uint32_t argb,
                                EaselFontDone done, gpointer data);

G_END_DECLS
