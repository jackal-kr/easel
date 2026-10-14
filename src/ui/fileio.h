/* Reading and writing picture files. Formats come from gdk-pixbuf (PNG, JPEG,
 * BMP, TIFF ...); the engine itself never sees a file. */
#pragma once

#include <gio/gio.h>

#include "core/document.h"

/* Alpha is flattened onto white: classic Paint has no transparency. */
EaselDocument *easel_file_load (GFile *file, GError **error);

/* The format follows the file extension (png, jpg/jpeg, bmp, tif/tiff, webp when the system can write it);
 * anything else is written as PNG. */
gboolean easel_file_save (const EaselDocument *doc, GFile *file, GError **error);

/* Can gdk-pixbuf write this type ("png", "webp", ...) on this system? */
gboolean easel_file_can_write (const char *type);

/* True if `name` ends in an extension easel_file_save understands. */
gboolean easel_file_has_known_extension (const char *name);
