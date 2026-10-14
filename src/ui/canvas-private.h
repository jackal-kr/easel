/* Stuff the canvas source files share with each other: canvas.c (drawing,
 * input, zoom, undo), canvas-selection.c, canvas-text.c, canvas-shapes.c and
 * canvas-transform.c. The widget is big, so it is split by feature, but they
 * all work on the same struct below. Outside code only sees canvas.h. */
#pragma once

#include "ui/canvas.h"
#include "ui/config.h"

#include <math.h>
#include <string.h>
#include <pango/pangocairo.h>

#include "core/clip.h"
#include "core/history.h"
#include "core/syncbuf.h"
#include "core/transform.h"
#include "ui/cursors.h"

#define HANDLE_PAD  8  /* logical px left free right of and below the picture for the resize handles */
#define HANDLE_SIZE 5

#define MAX_SEQ 512 /* most vertices a polygon can have */

/* How many pixel copies we keep for the GPU. GTK can still be reading one
 * from the last frame, and maybe a second from the one before, so three means
 * we nearly always find a free one without allocating. */
#define TEXTURE_BUFFERS 3

/* What the pointer is over on the text box. 1..8 are the resize handles, numbered
 * like the selection handles (see canvas_sel_handle_pos). */
enum { TEXT_HIT_NONE = 0, TEXT_HIT_BORDER = 9, TEXT_HIT_INSIDE = 10 };

/* What a drag that started on the text box is doing. */
enum { TEXT_DRAG_NONE = 0, TEXT_DRAG_MOVE, TEXT_DRAG_RESIZE, TEXT_DRAG_SELECT };

/* REVIEW: this struct has grown big. Selection, text and gradient state
 * could each move into their own sub-struct. */
struct _EaselCanvas
{
  GtkWidget parent_instance;

  /* ---- The picture and how it gets on screen ----
   * `doc` is the truth. The GPU texture is a copy that is refreshed lazily:
   * edits only record a dirty region, and the next snapshot uploads just that
   * region (see canvas.c, build_texture). */
  EaselDocument  *doc;
  GdkTexture     *texture;     /* last uploaded copy of doc */
  cairo_region_t *dirty;       /* pixels changed since `texture` was built */
  EaselSyncBuf   *shadow;      /* picture as of the last finished edit: the undo base */
  gboolean        shadow_taken;
  EaselSyncBuf   *tex_bufs[TEXTURE_BUFFERS]; /* pixel memory handed to textures, recycled */
  guint64         change_count; /* grows with every pixel change, so others can skip redraws */
  gint64          paint_us;     /* how long the last snapshot took (shown by Frame Time) */
  int             zoom;         /* percent, 100 = one picture pixel per device pixel */
  int             zoom_level;   /* what one Magnifier click zooms to */
  gboolean        center_pending;
  double          center_x, center_y; /* picture point to scroll to the middle after a zoom */

  /* ---- Colours and tool settings ---- */
  uint32_t        foreground;
  uint32_t        background;
  EaselTool       tool;
  int             tool_size;
  EaselBrushShape brush_shape;
  int             line_width;
  EaselFillStyle  fill_style;
  gboolean        eraser_opaque; /* eraser paints the background colour instead of transparency */
  gboolean        sel_opaque;    /* "Draw Opaque": background-coloured pixels in a selection are kept */

  /* ---- Gesture in progress (press .. drag .. release) ---- */
  gboolean  drawing;
  guint     button;           /* GDK_BUTTON_PRIMARY or _SECONDARY */
  uint32_t  stroke_color;     /* foreground for the left button, background for the right */
  uint32_t  picked;           /* Pick Color: colour under the pointer */
  double    start_x, start_y; /* widget coordinates where the drag began */
  int       start_ix, start_iy; /* the same, as a picture pixel */
  int       last_x, last_y;   /* last picture pixel handled, so strokes can be joined */
  int       hover_x, hover_y; /* pointer in picture pixels, -1 when outside */
  uint32_t *backup;           /* picture before a shape drag, so the preview can be redrawn */
  EaselRect shape_prev;       /* area the previewed shape currently covers */
  gboolean  panning;
  double    pan_x0, pan_y0, pan_h0, pan_v0; /* pointer and scroll position when panning began */

  /* ---- Polygon and curve tools (several clicks make one shape) ----
   * Polygon: seq[] holds the vertices. Curve: [0] start, [1] end, [2] and [3]
   * are the control points. */
  gboolean   seq_active;
  int        seq_stage;     /* curve: 0 drawing the line, 1 waiting for 1st bend, 2 for 2nd */
  int        seq_n;
  EaselPoint seq[MAX_SEQ];
  gboolean   seq_moved;
  gint64     last_press_us; /* for spotting a double click */

  /* ---- Grid overlay ---- */
  gboolean show_grid;
  int      grid_w, grid_h;  /* cell size in picture pixels */
  gboolean show_axo;        /* axonometric (2:1 isometric) lines */
  int      axo_w;           /* their horizontal spacing in picture pixels */

  /* ---- Handles for resizing the picture (right edge, bottom edge, corner) ---- */
  int      handle_hover;
  int      resize_handle;   /* 1 right, 2 bottom, 3 corner; 0 when not resizing */
  int      resize_w, resize_h;

  /* ---- Selection ----
   * The picture is only touched in two moments: when the selection is first
   * moved ("lifted": its pixels go into sel_clip and a hole is left behind)
   * and when it is committed (sel_clip is drawn back at its new place). In
   * between it just floats above the picture as a texture. */
  gboolean    sel_active;
  EaselRect   sel_rect;      /* current position and size, picture pixels */
  EaselClip  *sel_clip;      /* lifted contents, NULL until the first move */
  uint32_t   *sel_base;      /* picture before the lift: the undo base */
  EaselRect   sel_edit;      /* area the lift changed */
  GdkTexture *sel_texture;   /* sel_clip as drawn on screen */
  gboolean    sel_creating, sel_moving;
  int         sel_grab_x, sel_grab_y; /* where inside the selection it was grabbed */
  uint8_t    *sel_mask;      /* free-form shape inside sel_rect; NULL for a plain rectangle */
  GArray     *sel_path;      /* free-form outline (EaselPoint); NULL for a rectangle */
  int         sel_path_x, sel_path_y; /* sel_rect origin when the outline was drawn */
  EaselClip  *sel_src;       /* contents at the start of a stretch, so it always scales the original */
  EaselRect   sel_src_rect;
  int         sel_stretch;   /* handle being dragged (1..8), 0 none */

  /* ---- Gradient (stays adjustable until accepted) ---- */
  int       gradient_style;
  EaselRect grad_region;     /* what it fills: the whole picture or the selection */
  gboolean  grad_masked;
  gboolean  grad_pending;    /* drawn but not accepted yet: its end points can be dragged */
  int       grad_x0, grad_y0, grad_x1, grad_y1;
  int       grad_drag;       /* end point being dragged: 1 start, 2 end, 0 none */
  guint     grad_button;

  /* ---- Text tool ---- */
  gboolean     text_active, text_creating;
  EaselRect    text_rect;
  GString     *text_buf;
  int          text_cursor;   /* byte offset into text_buf */
  int          text_anchor;   /* other end of the text selection, -1 none */
  GArray      *text_attr;     /* one TextAttr per byte of text_buf */
  guint        text_typing;   /* style flags for text typed from now on */
  const char  *text_typing_family; /* font for text typed from now on (NULL: box font) */
  guint16      text_typing_size;
  char        *font;
  EaselClip   *text_clip;     /* rendered text, ready to be put on the picture */
  GdkTexture  *text_texture;
  PangoLayout *text_layout;
  gboolean     text_opaque, text_smooth;
  guint        text_blink_id;
  gboolean     text_caret_on;
  int          text_drag;     /* TEXT_DRAG_* */
  int          text_handle;   /* handle (1..8) while resizing */
  int          text_grab_x, text_grab_y;
  EaselRect    text_orig;

  /* ---- Right-click menus: 0 selection, 1 text box, 2 Pan / outside a selection ---- */
  GtkWidget  *ctx_popover[3];
  GMenuModel *ctx_model[3];
  int         ctx_pending;    /* menu to open when the button comes up; -1 none */
  double      ctx_x, ctx_y;
  EaselCanvasOversizeFunc oversize_func; /* asked what to do when a paste is bigger than the picture */
  gpointer    oversize_data;

  /* ---- Undo ---- */
  EaselHistory *history;
  EaselRect     edit_rect;  /* pixels touched by the edit in progress */
  uint32_t     *edit_base;  /* picture before a freehand edit (shape tools use `backup`) */

  /* ---- Airbrush ---- */
  guint    spray_source;    /* timer that keeps spraying while the button is held still */
  uint32_t spray_rng;
};

/* Style of one byte of text. `family` (interned, so it can be compared by
 * pointer and never freed) and `size` (points) are NULL / 0 for "the box font". */
typedef struct { guint8 flags; uint32_t color; const char *family; guint16 size; } TextAttr;

enum { SIGNAL_POINTER_MOVED, SIGNAL_COLOR_PICKED, SIGNAL_HISTORY_CHANGED, SIGNAL_RESIZE_PREVIEW, SIGNAL_ZOOM_CHANGED, SIGNAL_SELECTION_CHANGED, SIGNAL_TEXT_STATE, N_SIGNALS };
extern guint canvas_signals[N_SIGNALS];

/* Functions used by more than one of the canvas files. */
EaselRect canvas_apply_stroke (EaselCanvas *self, int x0, int y0, int x1, int y1);
EaselRect canvas_sel_rect_between (EaselCanvas *self, int x0, int y0, int x1, int y1);
GdkTexture * canvas_texture_from_clip (const EaselClip *clip, gboolean opaque, uint32_t key, int checker);
double canvas_logical_per_pixel (EaselCanvas *self);
gboolean canvas_sel_contains (EaselCanvas *self, int x, int y);
gboolean canvas_spray_tick (gpointer data);
gboolean canvas_text_key (EaselCanvas *self, guint keyval, GdkModifierType state);
gboolean canvas_text_sel_range (EaselCanvas *self, int *a, int *b);
int canvas_sel_handle_at (EaselCanvas *self, double wx, double wy);
int canvas_text_hit (EaselCanvas *self, double wx, double wy);
int canvas_text_index_at (EaselCanvas *self, int ix, int iy);
uint32_t * canvas_snapshot_take (EaselCanvas *self);
void canvas_begin_edit (EaselCanvas *self);
void canvas_cancel_seq (EaselCanvas *self);
void canvas_commit_seq (EaselCanvas *self, gboolean closed);
void canvas_draw_gradient_guide (EaselCanvas *self, GtkSnapshot *snapshot, double s);
void canvas_draw_selection (EaselCanvas *self, GtkSnapshot *snapshot, double s);
void canvas_draw_text (EaselCanvas *self, GtkSnapshot *snapshot, double s);
void canvas_end_seq (EaselCanvas *self);
void canvas_end_shape_drag (EaselCanvas *self, gboolean keep);
void canvas_finish_edit (EaselCanvas *self);
void canvas_grad_commit (EaselCanvas *self);
void canvas_grad_render (EaselCanvas *self);
void canvas_mark_all_dirty (EaselCanvas *self);
void canvas_mark_dirty (EaselCanvas *self, EaselRect r);
void canvas_push_undo (EaselCanvas *self, const uint32_t *base);
void canvas_restore_shape_area (EaselCanvas *self);
void canvas_sel_commit (EaselCanvas *self);
void canvas_sel_discard (EaselCanvas *self);
void canvas_sel_handle_pos (const EaselRect *r, int h, double s, double *wx, double *wy);
void canvas_sel_lift (EaselCanvas *self, gboolean copy);
void canvas_sel_notify (EaselCanvas *self);
void canvas_sel_refresh_texture (EaselCanvas *self);
void canvas_sel_stretch_to (EaselCanvas *self, int x, int y);
void canvas_seq_render (EaselCanvas *self, gboolean closed, gboolean rubber, int rx, int ry);
void canvas_snapshot_drop (EaselCanvas *self, uint32_t **snapshot);
void canvas_text_apply_color (EaselCanvas *self, uint32_t color);
void canvas_text_begin_drag (EaselCanvas *self, int x, int y);
void canvas_text_caret_restart (EaselCanvas *self);
void canvas_text_commit (EaselCanvas *self);
void canvas_text_drag_to (EaselCanvas *self, int x0, int y0, int x1, int y1);
void canvas_text_end_drag (EaselCanvas *self);
void canvas_text_free_render (EaselCanvas *self);
void canvas_text_move_cursor (EaselCanvas *self, int index, gboolean extend);
void canvas_text_render (EaselCanvas *self);
void canvas_text_resize_to (EaselCanvas *self, int x, int y);
void canvas_update_shape (EaselCanvas *self, int x, int y, gboolean constrain);

/* Mouse input, with plain numbers instead of GTK events so the tests can fake
 * it. (x, y) are widget coordinates in logical pixels, the offsets are
 * measured from where the button went down. */
void canvas_press (EaselCanvas *self, guint button, double x, double y, gboolean ctrl);
void canvas_drag (EaselCanvas *self, double offset_x, double offset_y, gboolean shift);
void canvas_release (EaselCanvas *self, double offset_x, double offset_y, gboolean shift);
