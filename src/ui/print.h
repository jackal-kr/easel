/* File > Print: prints the picture at actual size, shrunk to fit the page. */
#pragma once

#include <gtk/gtk.h>

#include "ui/canvas.h"

G_BEGIN_DECLS

void easel_print_picture (GtkWindow *parent, EaselCanvas *canvas);

G_END_DECLS
