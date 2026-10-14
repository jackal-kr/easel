#include "ui/fileio.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>

static const struct { const char *ext; const char *type; } formats[] = {
  { "png", "png" }, { "jpg", "jpeg" }, { "jpeg", "jpeg" }, { "bmp", "bmp" },
  { "tif", "tiff" }, { "tiff", "tiff" }, { "webp", "webp" },
};

static const char *
type_for_name (const char *name)
{
  const char *dot = strrchr (name, '.');

  if (dot != NULL)
    for (guint i = 0; i < G_N_ELEMENTS (formats); i++)
      if (g_ascii_strcasecmp (dot + 1, formats[i].ext) == 0)
        return formats[i].type;
  return NULL;
}

gboolean
easel_file_can_write (const char *type)
{
  for (GSList *l = gdk_pixbuf_get_formats (); l != NULL; l = l->next)
    {
      g_autofree char *name = gdk_pixbuf_format_get_name (l->data);

      if (g_str_equal (name, type))
        return gdk_pixbuf_format_is_writable (l->data);
    }
  return FALSE;
}

gboolean
easel_file_has_known_extension (const char *name)
{
  return type_for_name (name) != NULL;
}

EaselDocument *
easel_file_load (GFile *file, GError **error)
{
  g_autoptr (GFileInputStream) in = g_file_read (file, NULL, error);
  if (in == NULL)
    return NULL;

  g_autoptr (GdkPixbuf) pixbuf = gdk_pixbuf_new_from_stream (G_INPUT_STREAM (in), NULL, error);
  if (pixbuf == NULL)
    return NULL;

  int w = gdk_pixbuf_get_width (pixbuf), h = gdk_pixbuf_get_height (pixbuf);
  EaselDocument *doc = easel_document_new (w, h, EASEL_RGB (255, 255, 255));
  if (doc == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "The picture is too large (%d × %d)", w, h);
      return NULL;
    }

  int channels = gdk_pixbuf_get_n_channels (pixbuf);
  int stride = gdk_pixbuf_get_rowstride (pixbuf);
  const guchar *src = gdk_pixbuf_read_pixels (pixbuf);

  /* Keep transparency: pixels are stored premultiplied. */
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        const guchar *p = src + (gsize) y * stride + (gsize) x * channels;
        unsigned a = channels == 4 ? p[3] : 255;
        unsigned r = (p[0] * a + 127) / 255, g = (p[1] * a + 127) / 255, b = (p[2] * a + 127) / 255;

        doc->pixels[(gsize) y * w + x] = (a << 24) | (r << 16) | (g << 8) | b;
      }
  return doc;
}

gboolean
easel_file_save (const EaselDocument *doc, GFile *file, GError **error)
{
  g_autofree char *name = g_file_get_basename (file);
  const char *type = type_for_name (name);
  if (type == NULL || !easel_file_can_write (type))
    type = "png";

  /* PNG and TIFF keep transparency; JPEG and BMP are flattened onto white. */
  gboolean alpha = g_str_equal (type, "png") || g_str_equal (type, "tiff") || g_str_equal (type, "webp");
  int channels = alpha ? 4 : 3;
  g_autoptr (GdkPixbuf) pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, alpha, 8, doc->width, doc->height);
  if (pixbuf == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Not enough memory to save the picture");
      return FALSE;
    }

  int stride = gdk_pixbuf_get_rowstride (pixbuf);
  guchar *dst = gdk_pixbuf_get_pixels (pixbuf);
  for (int y = 0; y < doc->height; y++)
    for (int x = 0; x < doc->width; x++)
      {
        uint32_t c = doc->pixels[(gsize) y * doc->width + x];
        unsigned a = c >> 24, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
        guchar *p = dst + (gsize) y * stride + (gsize) x * channels;

        if (alpha)
          {
            /* Premultiplied -> straight colour. */
            if (a != 0 && a != 255)
              {
                r = MIN (255u, (r * 255 + a / 2) / a);
                g = MIN (255u, (g * 255 + a / 2) / a);
                b = MIN (255u, (b * 255 + a / 2) / a);
              }
            p[3] = (guchar) a;
          }
        else if (a != 255)
          {
            r = MIN (255u, r + (255 - a));
            g = MIN (255u, g + (255 - a));
            b = MIN (255u, b + (255 - a));
          }
        p[0] = (guchar) r;
        p[1] = (guchar) g;
        p[2] = (guchar) b;
      }

  /* Replace atomically so a failed save never destroys the old file. */
  g_autoptr (GFileOutputStream) out = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, error);
  if (out == NULL)
    return FALSE;

  gboolean ok = g_str_equal (type, "jpeg") || g_str_equal (type, "webp")
                  ? gdk_pixbuf_save_to_stream (pixbuf, G_OUTPUT_STREAM (out), type, NULL, error,
                                               "quality", g_str_equal (type, "jpeg") ? "95" : "90", NULL)
                  : gdk_pixbuf_save_to_stream (pixbuf, G_OUTPUT_STREAM (out), type, NULL, error, NULL);
  if (!ok)
    return FALSE;
  return g_output_stream_close (G_OUTPUT_STREAM (out), NULL, error);
}
