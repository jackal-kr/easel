/* Persistent preferences (GSettings). When the schema cannot be found the
 * getters return the given fallbacks and nothing is saved. */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The app's settings, or NULL if the schema is not installed. */
GSettings *easel_settings (void);

int       easel_pref_int (const char *key, int fallback);
gboolean  easel_pref_bool (const char *key, gboolean fallback);
char     *easel_pref_string (const char *key, const char *fallback); /* g_free */

/* Parses "#rrggbb"; returns FALSE for anything else. */
gboolean  easel_parse_hex_color (const char *text, uint32_t *argb);

/* Applies the color scheme and workspace color now and whenever they change. */
void      easel_settings_apply_appearance (void);

G_END_DECLS
