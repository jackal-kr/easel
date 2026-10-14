/* Dialogs that act on the picture: size and colors, zoom, flip and rotate,
 * stretch and skew. Each one applies its change to `canvas` on OK. */
#pragma once

#include <adwaita.h>

#include "ui/canvas.h"

G_BEGIN_DECLS

void easel_dialog_add_ok_cancel (AdwAlertDialog *dialog);
void easel_dialog_attributes (GtkWidget *parent, EaselCanvas *canvas);
void easel_dialog_custom_zoom (GtkWidget *parent, EaselCanvas *canvas);
void easel_dialog_flip_rotate (GtkWidget *parent, EaselCanvas *canvas);
void easel_dialog_grid (GtkWidget *parent, EaselCanvas *canvas);
void easel_dialog_stretch_skew (GtkWidget *parent, EaselCanvas *canvas);

G_END_DECLS
