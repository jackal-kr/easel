/* Bottom colour area: overlapping foreground/background swatch, swap button
 * and the 28-colour classic palette. Left-click picks the foreground colour,
 * right-click the background. Clicking a swatch opens the colour chooser. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define EASEL_TYPE_COLOR_BOX (easel_color_box_get_type ())
G_DECLARE_FINAL_TYPE (EaselColorBox, easel_color_box, EASEL, COLOR_BOX, GtkBox)

GtkWidget *easel_color_box_new (void);

uint32_t easel_color_box_get_foreground (EaselColorBox *self);
uint32_t easel_color_box_get_background (EaselColorBox *self);
void     easel_color_box_set_foreground (EaselColorBox *self, uint32_t argb);
void     easel_color_box_set_background (EaselColorBox *self, uint32_t argb);
void     easel_color_box_swap (EaselColorBox *self);
void     easel_color_box_edit_colors (EaselColorBox *self);

/* Signal "colors-changed" (no arguments). */

G_END_DECLS
