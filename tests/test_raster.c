/* Core tests: no GTK, no display needed. */
#include "core/document.h"
#include "core/raster.h"

#include <stdio.h>
#include <stdlib.h>

#define WHITE EASEL_RGB (255, 255, 255)
#define BLACK EASEL_RGB (0, 0, 0)
#define RED   EASEL_RGB (255, 0, 0)
#define BLUE  EASEL_RGB (0, 0, 255)

static int failures = 0;

#define CHECK(cond)                                                               \
  do {                                                                            \
    if (!(cond))                                                                  \
      {                                                                           \
        fprintf (stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        failures++;                                                               \
      }                                                                           \
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

static EaselBrush
brush (EaselBrushShape shape, int size, uint32_t color)
{
  return (EaselBrush) { shape, size, color };
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
test_brush_shapes (void)
{
  static const struct { EaselBrushShape shape; int size, pixels; } cases[] = {
    { EASEL_BRUSH_CIRCLE, 1, 1 },   { EASEL_BRUSH_CIRCLE, 2, 4 },
    { EASEL_BRUSH_CIRCLE, 3, 9 },   { EASEL_BRUSH_CIRCLE, 4, 12 },
    { EASEL_BRUSH_CIRCLE, 5, 21 },  { EASEL_BRUSH_CIRCLE, 8, 52 },
    { EASEL_BRUSH_SQUARE, 1, 1 },   { EASEL_BRUSH_SQUARE, 5, 25 },
    { EASEL_BRUSH_SQUARE, 8, 64 },  { EASEL_BRUSH_SLASH, 5, 5 },
    { EASEL_BRUSH_SLASH, 8, 8 },    { EASEL_BRUSH_BACKSLASH, 5, 5 },
    { EASEL_BRUSH_BACKSLASH, 2, 2 },
  };

  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
      EaselDocument *doc = easel_document_new (40, 40, WHITE);
      EaselBrush b = brush (cases[i].shape, cases[i].size, BLACK);
      EaselRect r = easel_raster_stamp (doc, 20, 20, &b);
      if (count_not (doc, WHITE) != cases[i].pixels)
        {
          fprintf (stderr, "shape %d size %d: expected %d pixels, got %d\n",
                   cases[i].shape, cases[i].size, cases[i].pixels, count_not (doc, WHITE));
          failures++;
        }
      CHECK (r.width == cases[i].size && r.height == cases[i].size);
      easel_document_free (doc);
    }

  /* Diagonal orientation: "/" runs bottom-left to top-right. */
  EaselDocument *doc = easel_document_new (20, 20, WHITE);
  EaselBrush slash = brush (EASEL_BRUSH_SLASH, 4, BLACK);
  easel_raster_stamp (doc, 10, 10, &slash); /* box is x 8..11, y 8..11 */
  CHECK (easel_document_get_pixel (doc, 8, 11) == BLACK);
  CHECK (easel_document_get_pixel (doc, 11, 8) == BLACK);
  CHECK (easel_document_get_pixel (doc, 8, 8) == WHITE);
  easel_document_free (doc);
}

static void
test_stamp_clipping (void)
{
  EaselDocument *doc = easel_document_new (10, 10, WHITE);
  EaselBrush b = brush (EASEL_BRUSH_CIRCLE, 5, BLACK);

  EaselRect r = easel_raster_stamp (doc, 0, 0, &b);
  CHECK (r.x == 0 && r.y == 0);
  CHECK (r.width > 0 && r.width <= 3 && r.height > 0 && r.height <= 3);
  CHECK (count_not (doc, WHITE) > 0);

  r = easel_raster_stamp (doc, -50, -50, &b);
  CHECK (r.width == 0 && r.height == 0);
  r = easel_raster_stamp (doc, 500, 500, &b);
  CHECK (r.width == 0 && r.height == 0);

  easel_document_free (doc);
}

static void
test_lines (void)
{
  EaselDocument *doc = easel_document_new (20, 20, WHITE);
  EaselBrush one = brush (EASEL_BRUSH_CIRCLE, 1, BLACK);

  easel_raster_stroke (doc, 5, 5, 5, 5, &one);
  CHECK (count_not (doc, WHITE) == 1);
  easel_document_fill (doc, WHITE);

  easel_raster_stroke (doc, 0, 5, 9, 5, &one);
  CHECK (count_not (doc, WHITE) == 10);
  for (int x = 0; x <= 9; x++)
    CHECK (easel_document_get_pixel (doc, x, 5) == BLACK);
  easel_document_fill (doc, WHITE);

  easel_raster_stroke (doc, 7, 2, 7, 11, &one);
  CHECK (count_not (doc, WHITE) == 10);
  easel_document_fill (doc, WHITE);

  easel_raster_stroke (doc, 0, 0, 9, 9, &one);
  CHECK (count_not (doc, WHITE) == 10);
  for (int i = 0; i <= 9; i++)
    CHECK (easel_document_get_pixel (doc, i, i) == BLACK);
  easel_document_fill (doc, WHITE);

  /* Steep line: exactly one pixel per row, no gaps. */
  easel_raster_stroke (doc, 0, 0, 3, 9, &one);
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

  easel_raster_stroke (doc, 9, 9, 0, 0, &one); /* reversed direction */
  CHECK (count_not (doc, WHITE) == 10);
  easel_document_fill (doc, WHITE);

  EaselBrush three = brush (EASEL_BRUSH_CIRCLE, 3, BLACK);
  EaselRect r = easel_raster_stroke (doc, -100, -100, -50, -50, &three);
  CHECK (r.width == 0);
  CHECK (count_not (doc, WHITE) == 0);
  easel_raster_stroke (doc, -5, 10, 25, 10, &one);
  CHECK (count_not (doc, WHITE) == 20);

  easel_document_free (doc);
}

static void
test_eraser (void)
{
  EaselDocument *doc = easel_document_new (30, 30, BLACK);

  /* Plain eraser paints the background over a size x size square. */
  EaselEraser e = { 4, WHITE, false, 0 };
  EaselRect r = easel_raster_erase (doc, 10, 10, &e);
  CHECK (count_not (doc, BLACK) == 16);
  CHECK (r.width == 4 && r.height == 4);
  CHECK (easel_document_get_pixel (doc, 8, 8) == WHITE);
  CHECK (easel_document_get_pixel (doc, 11, 11) == WHITE);
  CHECK (easel_document_get_pixel (doc, 12, 12) == BLACK);

  /* Color eraser: only pixels equal to from_color change. */
  easel_document_fill (doc, WHITE);
  for (int x = 0; x < 30; x += 2)
    easel_document_set_pixel (doc, x, 15, RED);
  EaselEraser ce = { 10, BLUE, true, RED };
  easel_raster_erase_stroke (doc, 0, 15, 29, 15, &ce);
  CHECK (count_not (doc, WHITE) == 15); /* the 15 red pixels became blue */
  CHECK (easel_document_get_pixel (doc, 0, 15) == BLUE);
  CHECK (easel_document_get_pixel (doc, 1, 15) == WHITE);
  CHECK (easel_document_get_pixel (doc, 3, 12) == WHITE);

  /* Entirely outside: no crash, empty rect. */
  r = easel_raster_erase (doc, -100, -100, &e);
  CHECK (r.width == 0);

  easel_document_free (doc);
}

static void
test_flood_fill (void)
{
  EaselDocument *doc = easel_document_new (20, 20, WHITE);

  /* Whole image. */
  EaselRect r = easel_raster_flood_fill (doc, 3, 3, RED);
  CHECK (count_not (doc, RED) == 0);
  CHECK (r.x == 0 && r.y == 0 && r.width == 20 && r.height == 20);

  /* Same colour and out-of-bounds are no-ops. */
  r = easel_raster_flood_fill (doc, 3, 3, RED);
  CHECK (r.width == 0);
  r = easel_raster_flood_fill (doc, -1, 3, BLUE);
  CHECK (r.width == 0);
  r = easel_raster_flood_fill (doc, 3, 20, BLUE);
  CHECK (r.width == 0);

  /* Closed box: fill inside only. Box outline spans x,y in 5..14. */
  easel_document_fill (doc, WHITE);
  for (int i = 5; i <= 14; i++)
    {
      easel_document_set_pixel (doc, i, 5, BLACK);
      easel_document_set_pixel (doc, i, 14, BLACK);
      easel_document_set_pixel (doc, 5, i, BLACK);
      easel_document_set_pixel (doc, 14, i, BLACK);
    }
  r = easel_raster_flood_fill (doc, 8, 8, BLUE);
  CHECK (count_not (doc, WHITE) == 36 + 64);    /* 36 border pixels + 8x8 inside */
  CHECK (easel_document_get_pixel (doc, 6, 6) == BLUE);
  CHECK (easel_document_get_pixel (doc, 13, 13) == BLUE);
  CHECK (easel_document_get_pixel (doc, 2, 2) == WHITE);
  CHECK (r.x == 6 && r.y == 6 && r.width == 8 && r.height == 8);

  /* Fill is 4-connected: a diagonal wall must not leak. */
  easel_document_fill (doc, WHITE);
  for (int i = 0; i < 20; i++)
    easel_document_set_pixel (doc, i, i, BLACK);
  easel_raster_flood_fill (doc, 0, 10, RED); /* below the diagonal: x < y */
  CHECK (count_not (doc, WHITE) == 20 + 190);
  CHECK (easel_document_get_pixel (doc, 10, 0) == WHITE);
  CHECK (easel_document_get_pixel (doc, 0, 10) == RED);

  easel_document_free (doc);

  /* Big image with a serpentine wall: must not blow the stack or be slow. */
  doc = easel_document_new (1500, 1500, WHITE);
  for (int y = 2; y < 1500; y += 4)
    for (int x = 0; x < 1499; x++)
      easel_document_set_pixel (doc, (y / 4) % 2 ? x + 1 : x, y, BLACK);
  easel_raster_flood_fill (doc, 0, 0, RED);
  CHECK (easel_document_get_pixel (doc, 0, 0) == RED);
  CHECK (count_not (doc, WHITE) > 1000000);
  easel_document_free (doc);
}

int
main (void)
{
  test_document ();
  test_brush_shapes ();
  test_stamp_clipping ();
  test_lines ();
  test_eraser ();
  test_flood_fill ();

  if (failures != 0)
    {
      fprintf (stderr, "%d check(s) failed\n", failures);
      return EXIT_FAILURE;
    }
  puts ("raster: all checks passed");
  return EXIT_SUCCESS;
}
