#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define EASEL_TYPE_WINDOW (easel_window_get_type ())
G_DECLARE_FINAL_TYPE (EaselWindow, easel_window, EASEL, WINDOW, AdwApplicationWindow)

EaselWindow *easel_window_new (GtkApplication *app);

G_END_DECLS
