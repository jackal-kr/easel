/* Core tests: no GTK, no display needed. */
#include "core/document.h"
#include "core/raster.h"

#include <stdio.h>
#include <stdlib.h>

#define WHITE EASEL_RGB (255, 255, 255)
#define BLACK EASEL_RGB (0, 0, 0)

static int failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond))                                                             \
      {                                                                      \
        fprintf (stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        failures++;                                                          \
      }                                                                      \
  } while (0)

static int
count_not (const EaselDocument *doc, uint32_t background)
{
  int n = 0;
  for (int i = 0; i < doc->width * doc->height; i++)
    if (doc->pixels[i] != background)
      n++;
  return n;
}

static void
test_document (void)
{
  CHECK (easel_document_new (0, 10, WHITE) == NULL);
  CHECK (easel_document_new (10, -1, WHITE) == NULL);
  CHECK (easel_document_new (100000, 100000, WHITE) == NULL);

  EaselDocument *doc = easel_document_new (10, 8, WHITE);
  CHECK (doc != NULL);
  CHECK (easel_document_get_pixel (doc, 3, 3) == WHITE);
  CHECK (easel_document_get_pixel (doc, -1, 0) == 0);
  CHECK (easel_document_get_pixel (doc, 10, 0) == 0);
  easel_document_set_pixel (doc, 99, 99, BLACK); /* must not crash */
  easel_document_set_pixel (doc, 2, 2, BLACK);
  CHECK (easel_document_get_pixel (doc, 2, 2) == BLACK);
  CHECK (count_not (doc, WHITE) == 1);
  easel_document_fill (doc, BLACK);
  CHECK (count_not (doc, BLACK) == 0);
  easel_document_free (doc);
}

static void
test_stamp_sizes (void)
{
  static const struct { int size, pixels; } cases[] = {
    { 1, 1 }, { 2, 4 }, { 3, 9 }, { 4, 12 }, { 5, 21 }, { 8, 52 },
  };

  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
      EaselDocument *doc = easel_document_new (40, 40, WHITE);
      EaselRect r = easel_raster_stamp (doc, 20, 20, cases[i].size, BLACK);
      if (count_not (doc, WHITE) != cases[i].pixels)
        {
          fprintf (stderr, "stamp size %d: expected %d pixels, got %d\n",
                   cases[i].size, cases[i].pixels, count_not (doc, WHITE));
          failures++;
        }
      CHECK (r.width == cases[i].size && r.height == cases[i].size);
      easel_document_free (doc);
    }
}

static void
test_stamp_clipping (void)
{
  EaselDocument *doc = easel_document_new (10, 10, WHITE);

  EaselRect r = easel_raster_stamp (doc, 0, 0, 5, BLACK);
  CHECK (r.x == 0 && r.y == 0);
  CHECK (r.width > 0 && r.width <= 3 && r.height > 0 && r.height <= 3);
  CHECK (count_not (doc, WHITE) > 0);

  r = easel_raster_stamp (doc, -50, -50, 5, BLACK);
  CHECK (r.width == 0 && r.height == 0);
  r = easel_raster_stamp (doc, 500, 500, 5, BLACK);
  CHECK (r.width == 0 && r.height == 0);

  easel_document_free (doc);
}

static void
test_lines (void)
{
  EaselDocument *doc = easel_document_new (20, 20, WHITE);

  /* Single point. */
  easel_raster_stroke (doc, 5, 5, 5, 5, 1, BLACK);
  CHECK (count_not (doc, WHITE) == 1);
  easel_document_fill (doc, WHITE);

  /* Horizontal and vertical: one pixel per step, both ends included. */
  easel_raster_stroke (doc, 0, 5, 9, 5, 1, BLACK);
  CHECK (count_not (doc, WHITE) == 10);
  for (int x = 0; x <= 9; x++)
    CHECK (easel_document_get_pixel (doc, x, 5) == BLACK);
  easel_document_fill (doc, WHITE);

  easel_raster_stroke (doc, 7, 2, 7, 11, 1, BLACK);
  CHECK (count_not (doc, WHITE) == 10);
  easel_document_fill (doc, WHITE);

  /* Exact diagonal. */
  easel_raster_stroke (doc, 0, 0, 9, 9, 1, BLACK);
  CHECK (count_not (doc, WHITE) == 10);
  for (int i = 0; i <= 9; i++)
    CHECK (easel_document_get_pixel (doc, i, i) == BLACK);
  easel_document_fill (doc, WHITE);

  /* Steep line: exactly one pixel per row, no gaps. */
  easel_raster_stroke (doc, 0, 0, 3, 9, 1, BLACK);
  CHECK (count_not (doc, WHITE) == 10);
  for (int y = 0; y <= 9; y++)
    {
      int in_row = 0;
      for (int x = 0; x < 20; x++)
        if (easel_document_get_pixel (doc, x, y) == BLACK)
          in_row++;
      CHECK (in_row == 1);
    }
  easel_document_fill (doc, WHITE);

  /* Reversed direction draws the same pixels. */
  easel_raster_stroke (doc, 9, 9, 0, 0, 1, BLACK);
  CHECK (count_not (doc, WHITE) == 10);
  easel_document_fill (doc, WHITE);

  /* Wholly and partly outside: must not crash, must clip. */
  EaselRect r = easel_raster_stroke (doc, -100, -100, -50, -50, 3, BLACK);
  CHECK (r.width == 0);
  CHECK (count_not (doc, WHITE) == 0);
  easel_raster_stroke (doc, -5, 10, 25, 10, 1, BLACK);
  CHECK (count_not (doc, WHITE) == 20);

  easel_document_free (doc);
}

int
main (void)
{
  test_document ();
  test_stamp_sizes ();
  test_stamp_clipping ();
  test_lines ();

  if (failures != 0)
    {
      fprintf (stderr, "%d check(s) failed\n", failures);
      return EXIT_FAILURE;
    }
  puts ("raster: all checks passed");
  return EXIT_SUCCESS;
}
