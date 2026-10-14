/* Preferences dialog: appearance, new pictures, drawing defaults, palette.
 * Every change is saved at once (GSettings); there is no OK button. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

void easel_preferences_present (GtkWidget *parent);

G_END_DECLS
