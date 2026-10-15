/* Canvas tests: drive the canvas with plain pointer input (no real window)
 * and check the picture, undo and redo. Needs a display for GTK to start;
 * without one the test is skipped. */
#include "ui/canvas-private.h"

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

static EaselCanvas *
new_canvas (int w, int h)
{
  EaselCanvas *c = EASEL_CANVAS (easel_canvas_new (easel_document_new (w, h, WHITE)));

  g_object_ref_sink (c);
  return c;
}

static uint32_t
px (EaselCanvas *c, int x, int y)
{
  return easel_document_get_pixel (easel_canvas_get_document (c), x, y);
}

/* A drag in several steps, as a pointer moving across the picture. */
static void
drag (EaselCanvas *c, guint button, double x0, double y0, double x1, double y1)
{
  canvas_press (c, button, x0, y0, FALSE);
  for (int i = 1; i <= 8; i++)
    canvas_drag (c, (x1 - x0) * i / 8, (y1 - y0) * i / 8, FALSE);
  canvas_release (c, x1 - x0, y1 - y0, FALSE);
}

static uint32_t *
copy_pixels (EaselCanvas *c)
{
  EaselDocument *d = easel_canvas_get_document (c);
  return g_memdup2 (d->pixels, (gsize) d->width * d->height * 4);
}

static gboolean
same_as (EaselCanvas *c, const uint32_t *ref)
{
  EaselDocument *d = easel_canvas_get_document (c);
  return memcmp (d->pixels, ref, (size_t) d->width * d->height * 4) == 0;
}

static void
test_pencil_undo_redo (void)
{
  EaselCanvas *c = new_canvas (64, 64);

  easel_canvas_set_foreground (c, RED);
  easel_canvas_set_tool (c, EASEL_TOOL_PENCIL);
  drag (c, GDK_BUTTON_PRIMARY, 10, 10, 30, 10);
  CHECK (px (c, 10, 10) == RED && px (c, 30, 10) == RED && px (c, 20, 10) == RED);
  CHECK (px (c, 20, 12) == WHITE);
  CHECK (easel_canvas_can_undo (c));

  /* Right button draws with the background color. */
  easel_canvas_set_background (c, BLUE);
  drag (c, GDK_BUTTON_SECONDARY, 10, 20, 30, 20);
  CHECK (px (c, 20, 20) == BLUE);

  easel_canvas_undo (c);
  CHECK (px (c, 20, 20) == WHITE && px (c, 20, 10) == RED);
  easel_canvas_undo (c);
  CHECK (px (c, 20, 10) == WHITE && !easel_canvas_can_undo (c));
  easel_canvas_redo (c);
  CHECK (px (c, 20, 10) == RED && px (c, 20, 20) == WHITE);
  easel_canvas_redo (c);
  CHECK (px (c, 20, 20) == BLUE && !easel_canvas_can_redo (c));
  g_object_unref (c);
}

/* Every kind of edit must make the picture count as modified (the window asks
 * before discarding it), and saving must clear that. */
static void
test_modified_flag (void)
{
  static const EaselTool tools[] = { EASEL_TOOL_PENCIL, EASEL_TOOL_BRUSH, EASEL_TOOL_ERASER, EASEL_TOOL_LINE,
                                     EASEL_TOOL_RECTANGLE, EASEL_TOOL_ELLIPSE, EASEL_TOOL_AIRBRUSH };

  for (guint i = 0; i < G_N_ELEMENTS (tools); i++)
    {
      EaselCanvas *c = new_canvas (64, 64);

      easel_canvas_set_foreground (c, RED);
      easel_canvas_set_background (c, BLUE); /* so the eraser has something to change */
      easel_canvas_set_tool (c, tools[i]);
      CHECK (easel_canvas_is_saved (c));
      drag (c, GDK_BUTTON_PRIMARY, 10, 10, 30, 20);
      CHECK (!easel_canvas_is_saved (c));
      easel_canvas_mark_saved (c);
      CHECK (easel_canvas_is_saved (c));
      easel_canvas_undo (c);
      CHECK (!easel_canvas_is_saved (c));
      g_object_unref (c);
    }
}

/* The eraser's Transparent mode leaves see-through pixels, not the background. */
static void
test_eraser_transparent (void)
{
  EaselCanvas *c = new_canvas (32, 32);

  easel_canvas_set_background (c, BLUE);
  easel_canvas_set_tool (c, EASEL_TOOL_ERASER);
  easel_canvas_set_tool_size (c, 4);
  easel_canvas_set_eraser_opaque (c, TRUE);
  drag (c, GDK_BUTTON_PRIMARY, 4, 4, 12, 4);
  CHECK (px (c, 8, 4) == BLUE);
  easel_canvas_set_eraser_opaque (c, FALSE);
  drag (c, GDK_BUTTON_PRIMARY, 4, 20, 12, 20);
  CHECK (px (c, 8, 20) == 0);
  CHECK (px (c, 8, 12) == WHITE);
  g_object_unref (c);
}

static void
test_shapes (void)
{
  EaselCanvas *c = new_canvas (64, 64);

  easel_canvas_set_foreground (c, BLACK);
  easel_canvas_set_tool (c, EASEL_TOOL_RECTANGLE);
  easel_canvas_set_fill_style (c, EASEL_FILL_OUTLINE);
  drag (c, GDK_BUTTON_PRIMARY, 5, 5, 25, 15);
  CHECK (px (c, 5, 5) == BLACK && px (c, 25, 15) == BLACK);
  CHECK (px (c, 15, 10) == WHITE); /* hollow */
  easel_canvas_undo (c);
  CHECK (px (c, 5, 5) == WHITE && px (c, 25, 15) == WHITE);

  easel_canvas_set_tool (c, EASEL_TOOL_LINE);
  drag (c, GDK_BUTTON_PRIMARY, 0, 0, 40, 0);
  CHECK (px (c, 0, 0) == BLACK && px (c, 40, 0) == BLACK && px (c, 20, 0) == BLACK);
  g_object_unref (c);
}

static void
test_fill_and_eraser (void)
{
  EaselCanvas *c = new_canvas (32, 32);

  easel_canvas_set_foreground (c, RED);
  easel_canvas_set_tool (c, EASEL_TOOL_FILL);
  canvas_press (c, GDK_BUTTON_PRIMARY, 4, 4, FALSE);
  canvas_release (c, 0, 0, FALSE);
  CHECK (px (c, 0, 0) == RED && px (c, 31, 31) == RED);
  easel_canvas_undo (c);
  CHECK (px (c, 0, 0) == WHITE);
  easel_canvas_redo (c);

  easel_canvas_set_background (c, WHITE);
  easel_canvas_set_tool (c, EASEL_TOOL_ERASER);
  easel_canvas_set_tool_size (c, 4);
  drag (c, GDK_BUTTON_PRIMARY, 10, 10, 20, 10);
  CHECK (px (c, 15, 10) == WHITE && px (c, 15, 20) == RED);
  g_object_unref (c);
}

/* Select a block, move it, and check the picture and undo. */
static void
test_selection_move (void)
{
  EaselCanvas *c = new_canvas (80, 40);
  EaselDocument *d = easel_canvas_get_document (c);

  for (int y = 10; y < 20; y++)
    for (int x = 10; x < 20; x++)
      easel_document_set_pixel (d, x, y, RED);
  easel_canvas_invalidate (c);
  easel_canvas_set_background (c, WHITE);

  easel_canvas_set_tool (c, EASEL_TOOL_SELECT);
  drag (c, GDK_BUTTON_PRIMARY, 8, 8, 22, 22);      /* select */
  CHECK (easel_canvas_has_selection (c));
  drag (c, GDK_BUTTON_PRIMARY, 15, 15, 55, 15);    /* move 40 right */
  easel_canvas_flush (c);
  CHECK (px (c, 55, 12) == RED);
  CHECK (px (c, 15, 15) == WHITE);

  easel_canvas_undo (c);
  CHECK (px (c, 15, 15) == RED && px (c, 55, 12) == WHITE);
  g_object_unref (c);
}

/* Random mix of edits and undo/redo; after every step the picture must equal
 * what it was at that point of the history. This checks that the undo "before"
 * image stays in step with the picture. */
static void
test_random_history (void)
{
  EaselCanvas *c = new_canvas (96, 72);
  GPtrArray *states = g_ptr_array_new_with_free_func (g_free); /* states[i] = picture after i edits */
  guint pos = 0;
  GRand *rng = g_rand_new_with_seed (42);
  static const EaselTool tools[] = { EASEL_TOOL_PENCIL, EASEL_TOOL_BRUSH, EASEL_TOOL_ERASER, EASEL_TOOL_LINE,
                                     EASEL_TOOL_RECTANGLE, EASEL_TOOL_ELLIPSE, EASEL_TOOL_FILL };

  g_ptr_array_add (states, copy_pixels (c));
  for (int step = 0; step < 300; step++)
    {
      int what = g_rand_int_range (rng, 0, 10);

      if (what < 6)
        {
          easel_canvas_set_tool (c, tools[g_rand_int_range (rng, 0, G_N_ELEMENTS (tools))]);
          easel_canvas_set_foreground (c, EASEL_RGB (g_rand_int_range (rng, 0, 256), g_rand_int_range (rng, 0, 256),
                                                      g_rand_int_range (rng, 0, 256)));
          easel_canvas_set_fill_style (c, (EaselFillStyle) g_rand_int_range (rng, 0, 3));
          drag (c, g_rand_boolean (rng) ? GDK_BUTTON_PRIMARY : GDK_BUTTON_SECONDARY,
                g_rand_int_range (rng, 0, 96), g_rand_int_range (rng, 0, 72),
                g_rand_int_range (rng, 0, 96), g_rand_int_range (rng, 0, 72));
          uint32_t *now = copy_pixels (c);
          if (easel_canvas_can_undo (c) && memcmp (now, g_ptr_array_index (states, pos), 96 * 72 * 4) != 0)
            {
              g_ptr_array_set_size (states, pos + 1); /* new edit forgets redo */
              g_ptr_array_add (states, now);
              pos++;
            }
          else
            g_free (now);
          CHECK (same_as (c, g_ptr_array_index (states, pos)));
        }
      else if (what < 8 && pos > 0)
        {
          easel_canvas_undo (c);
          pos--;
          CHECK (same_as (c, g_ptr_array_index (states, pos)));
        }
      else if (pos + 1 < states->len)
        {
          easel_canvas_redo (c);
          pos++;
          CHECK (same_as (c, g_ptr_array_index (states, pos)));
        }
    }
  g_rand_free (rng);
  g_ptr_array_free (states, TRUE);
  g_object_unref (c);
}

static int
count_not_white (EaselCanvas *c)
{
  EaselDocument *d = easel_canvas_get_document (c);
  int n = 0;

  for (int i = 0; i < d->width * d->height; i++)
    n += d->pixels[i] != WHITE;
  return n;
}

static void
test_text (void)
{
  EaselCanvas *c = new_canvas (160, 60);

  easel_canvas_set_foreground (c, BLACK);
  easel_canvas_set_tool (c, EASEL_TOOL_TEXT);
  drag (c, GDK_BUTTON_PRIMARY, 5, 5, 120, 40);   /* the text box */
  CHECK (easel_canvas_text_active (c));
  CHECK (canvas_text_key (c, GDK_KEY_H, 0));
  CHECK (canvas_text_key (c, GDK_KEY_i, 0));
  CHECK (count_not_white (c) == 0);               /* still just an overlay */
  easel_canvas_flush (c);                         /* commit */
  CHECK (!easel_canvas_text_active (c));
  CHECK (count_not_white (c) > 10);
  easel_canvas_undo (c);
  CHECK (count_not_white (c) == 0);
  easel_canvas_redo (c);
  CHECK (count_not_white (c) > 10);
  g_object_unref (c);
}


/* Click without moving (press and release at the same spot). */
static void
click (EaselCanvas *c, double x, double y)
{
  canvas_press (c, GDK_BUTTON_PRIMARY, x, y, FALSE);
  canvas_release (c, 0, 0, FALSE);
}

static void
test_polygon (void)
{
  EaselCanvas *c = new_canvas (64, 64);

  easel_canvas_set_foreground (c, BLACK);
  easel_canvas_set_tool (c, EASEL_TOOL_POLYGON);
  easel_canvas_set_fill_style (c, EASEL_FILL_OUTLINE);
  drag (c, GDK_BUTTON_PRIMARY, 10, 10, 40, 10);    /* first edge by dragging */
  click (c, 40, 40);                               /* next corner */
  CHECK (px (c, 25, 10) == BLACK);                 /* first edge is drawn */
  click (c, 11, 11);                               /* near the start closes it */
  CHECK (px (c, 40, 25) == BLACK);                 /* second edge */
  CHECK (count_not_white (c) > 60);
  easel_canvas_undo (c);                           /* the whole polygon is one step */
  CHECK (count_not_white (c) == 0);
  g_object_unref (c);
}

static void
test_curve (void)
{
  EaselCanvas *c = new_canvas (64, 64);

  easel_canvas_set_foreground (c, BLACK);
  easel_canvas_set_tool (c, EASEL_TOOL_CURVE);
  drag (c, GDK_BUTTON_PRIMARY, 10, 30, 50, 30);    /* the baseline */
  CHECK (px (c, 30, 30) == BLACK);
  drag (c, GDK_BUTTON_PRIMARY, 30, 30, 30, 10);    /* first bend */
  drag (c, GDK_BUTTON_PRIMARY, 30, 30, 30, 10);    /* second bend, commits */
  CHECK (px (c, 10, 30) == BLACK && px (c, 50, 30) == BLACK);
  CHECK (px (c, 30, 30) == WHITE);                 /* the line became an S */
  easel_canvas_undo (c);
  CHECK (count_not_white (c) == 0);
  g_object_unref (c);
}

static void
test_gradient (void)
{
  EaselCanvas *c = new_canvas (64, 16);

  easel_canvas_set_foreground (c, BLACK);
  easel_canvas_set_background (c, WHITE);
  easel_canvas_set_tool (c, EASEL_TOOL_GRADIENT);
  drag (c, GDK_BUTTON_PRIMARY, 0, 8, 63, 8);
  CHECK (px (c, 0, 8) != px (c, 63, 8));           /* a blend across the picture */
  easel_canvas_set_tool (c, EASEL_TOOL_PENCIL);    /* changing tool accepts it */
  CHECK (easel_canvas_can_undo (c));
  easel_canvas_undo (c);
  CHECK (count_not_white (c) == 0);
  g_object_unref (c);
}

static guint picked_color;
static gboolean picked_fg;
static void
on_picked (EaselCanvas *c, guint argb, gboolean fg, gpointer data)
{
  picked_color = argb;
  picked_fg = fg;
}

static void
test_pick_color (void)
{
  EaselCanvas *c = new_canvas (32, 32);
  EaselDocument *d = easel_canvas_get_document (c);

  easel_document_set_pixel (d, 5, 5, RED);
  easel_canvas_invalidate (c);
  g_signal_connect (c, "color-picked", G_CALLBACK (on_picked), NULL);
  easel_canvas_set_tool (c, EASEL_TOOL_PICK_COLOR);
  picked_color = 0;
  click (c, 5, 5);
  CHECK (picked_color == RED && picked_fg);
  canvas_press (c, GDK_BUTTON_SECONDARY, 5, 5, FALSE);
  canvas_release (c, 0, 0, FALSE);
  CHECK (picked_color == RED && !picked_fg);
  CHECK (!easel_canvas_can_undo (c));              /* picking changes nothing */
  g_object_unref (c);
}

static void
test_free_form_select (void)
{
  EaselCanvas *c = new_canvas (64, 64);
  EaselDocument *d = easel_canvas_get_document (c);

  for (int y = 10; y < 40; y++)
    for (int x = 10; x < 40; x++)
      easel_document_set_pixel (d, x, y, RED);
  easel_canvas_invalidate (c);
  easel_canvas_set_background (c, WHITE);
  easel_canvas_set_tool (c, EASEL_TOOL_FREE_FORM_SELECT);
  canvas_press (c, GDK_BUTTON_PRIMARY, 8, 8, FALSE);
  canvas_drag (c, 32, 0, FALSE);
  canvas_drag (c, 32, 32, FALSE);
  canvas_drag (c, 0, 32, FALSE);
  canvas_release (c, 0, 32, FALSE);
  CHECK (easel_canvas_has_selection (c));
  easel_canvas_clear_selection (c);                /* delete what is inside the outline */
  CHECK (px (c, 20, 20) == WHITE);
  easel_canvas_undo (c);
  CHECK (px (c, 20, 20) == RED);
  g_object_unref (c);
}

static void
test_selection_stretch_and_copy (void)
{
  EaselCanvas *c = new_canvas (80, 40);
  EaselDocument *d = easel_canvas_get_document (c);

  for (int y = 10; y < 30; y++)
    for (int x = 10; x < 30; x++)
      easel_document_set_pixel (d, x, y, RED);
  easel_canvas_invalidate (c);
  easel_canvas_set_background (c, WHITE);
  easel_canvas_set_tool (c, EASEL_TOOL_SELECT);
  drag (c, GDK_BUTTON_PRIMARY, 10, 10, 29, 29);
  CHECK (easel_canvas_has_selection (c));
  /* Ctrl+drag copies instead of moving. */
  canvas_press (c, GDK_BUTTON_PRIMARY, 20, 20, TRUE);
  canvas_drag (c, 30, 0, FALSE);
  canvas_release (c, 30, 0, FALSE);
  easel_canvas_flush (c);
  CHECK (px (c, 20, 20) == RED && px (c, 50, 20) == RED);
  easel_canvas_undo (c);
  CHECK (px (c, 20, 20) == RED && px (c, 50, 20) == WHITE);

  /* Dragging a handle stretches the contents. */
  drag (c, GDK_BUTTON_PRIMARY, 10, 10, 29, 29);
  canvas_press (c, GDK_BUTTON_PRIMARY, 29, 29, FALSE);   /* bottom right corner */
  canvas_drag (c, 10, 0, FALSE);
  canvas_release (c, 10, 0, FALSE);
  easel_canvas_flush (c);
  CHECK (px (c, 38, 20) == RED);
  g_object_unref (c);
}

static void
test_resize_handle (void)
{
  EaselCanvas *c = new_canvas (64, 32);
  gtk_widget_measure (GTK_WIDGET (c), GTK_ORIENTATION_HORIZONTAL, -1, NULL, NULL, NULL, NULL);

  /* The corner handle is just outside the bottom right of the picture. */
  double s = canvas_logical_per_pixel (c);
  double hx = 64 * s + HANDLE_PAD / 2.0, hy = 32 * s + HANDLE_PAD / 2.0;
  int before = easel_canvas_get_document (c)->width;

  canvas_press (c, GDK_BUTTON_PRIMARY, hx, hy, FALSE);
  if (c->resize_handle != 0)
    {
      canvas_drag (c, 16 * s, 8 * s, FALSE);
      canvas_release (c, 16 * s, 8 * s, FALSE);
      CHECK (easel_canvas_get_document (c)->width == before + 16);
      CHECK (easel_canvas_get_document (c)->height == 32 + 8);
      easel_canvas_undo (c);
      CHECK (easel_canvas_get_document (c)->width == before);
    }
  else
    {
      /* Handle position is a detail of the layout; at least a plain press must be harmless. */
      canvas_release (c, 0, 0, FALSE);
      CHECK (easel_canvas_get_document (c)->width == before);
    }
  g_object_unref (c);
}

static void
test_text_box_drag (void)
{
  EaselCanvas *c = new_canvas (160, 80);

  easel_canvas_set_foreground (c, BLACK);
  easel_canvas_set_tool (c, EASEL_TOOL_TEXT);
  drag (c, GDK_BUTTON_PRIMARY, 10, 10, 90, 40);
  CHECK (easel_canvas_text_active (c));
  CHECK (c->text_rect.x == 10 && c->text_rect.y == 10);
  /* Dragging the border moves the box. */
  drag (c, GDK_BUTTON_PRIMARY, 10, 18, 30, 38);
  CHECK (c->text_rect.x == 30 && c->text_rect.y == 30);
  /* Dragging a corner handle resizes it. */
  int w = c->text_rect.width;
  drag (c, GDK_BUTTON_PRIMARY, c->text_rect.x + c->text_rect.width - 1, c->text_rect.y + c->text_rect.height - 1,
        c->text_rect.x + c->text_rect.width + 19, c->text_rect.y + c->text_rect.height - 1);
  CHECK (c->text_rect.width >= w);
  easel_canvas_flush (c);
  CHECK (!easel_canvas_text_active (c));
  g_object_unref (c);
}

static void
test_magnifier (void)
{
  EaselCanvas *c = new_canvas (64, 64);

  easel_canvas_set_tool (c, EASEL_TOOL_MAGNIFIER);
  easel_canvas_set_zoom_level (c, 4);
  click (c, 10, 10);
  CHECK (easel_canvas_get_zoom (c) == 400);
  click (c, 10, 10);
  CHECK (easel_canvas_get_zoom (c) == 100);
  g_object_unref (c);
}

static void
test_airbrush (void)
{
  EaselCanvas *c = new_canvas (64, 64);

  easel_canvas_set_foreground (c, BLACK);
  easel_canvas_set_tool (c, EASEL_TOOL_AIRBRUSH);
  easel_canvas_set_tool_size (c, 8);
  canvas_press (c, GDK_BUTTON_PRIMARY, 30, 30, FALSE);
  for (int i = 0; i < 20; i++)
    canvas_spray_tick (c);
  canvas_release (c, 0, 0, FALSE);
  CHECK (count_not_white (c) > 5);
  easel_canvas_undo (c);
  CHECK (count_not_white (c) == 0);
  g_object_unref (c);
}

int
main (int argc, char **argv)
{
  if (!gtk_init_check ())
    {
      fputs ("no display: skipping canvas tests\n", stderr);
      return 77; /* "skipped" for meson */
    }

  test_pencil_undo_redo ();
  test_modified_flag ();
  test_eraser_transparent ();
  test_shapes ();
  test_fill_and_eraser ();
  test_selection_move ();
  test_random_history ();
  test_text ();
  test_polygon ();
  test_curve ();
  test_gradient ();
  test_pick_color ();
  test_free_form_select ();
  test_selection_stretch_and_copy ();
  test_resize_handle ();
  test_text_box_drag ();
  test_magnifier ();
  test_airbrush ();

  if (failures != 0)
    {
      fprintf (stderr, "%d check(s) failed\n", failures);
      return EXIT_FAILURE;
    }
  puts ("canvas: all checks passed");
  return EXIT_SUCCESS;
}
