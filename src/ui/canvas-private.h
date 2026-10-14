/* Internal declarations shared by the canvas source files (canvas.c,
 * canvas-selection.c, canvas-text.c, canvas-shapes.c, canvas-transform.c).
 * Not part of the public canvas API: see canvas.h for that. */
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

#define HANDLE_PAD  8  /* logical px reserved right/below the picture for resize handles */
#define HANDLE_SIZE 5

#define MAX_SEQ 512 /* vertices of a polygon in progress */

#define TEXTURE_BUFFERS 3

/* Where a pointer is relative to the text box. Values 1..8 are the resize
 * handles (same numbering as the selection handles). */
enum { TEXT_HIT_NONE = 0, TEXT_HIT_BORDER = 9, TEXT_HIT_INSIDE = 10 };

/* What a drag on the text box is doing. */
enum { TEXT_DRAG_NONE = 0, TEXT_DRAG_MOVE, TEXT_DRAG_RESIZE, TEXT_DRAG_SELECT };

struct _EaselCanvas
{
  GtkWidget parent_instance;

  EaselDocument  *doc;
  GdkTexture     *texture; /* last uploaded snapshot of doc */
  cairo_region_t *dirty;   /* image pixels changed since `texture` was built */

  EaselSyncBuf *shadow;       /* the picture as of the last committed edit: the undo base */
  gboolean shadow_taken;
  EaselSyncBuf *tex_bufs[TEXTURE_BUFFERS]; /* pixel copies handed to textures */
  guint64  change_count; /* grows with every pixel change; lets others skip redraws */
  gint64   paint_us; /* time the last snapshot took (CPU side) */
  int      zoom; /* magnification in percent, 100 = actual size */
  uint32_t foreground;
  uint32_t background;

  EaselTool       tool;
  int             tool_size;
  EaselBrushShape brush_shape;
  int             line_width;
  EaselFillStyle  fill_style;

  /* Gesture in progress. */
  gboolean drawing;
  guint    button;       /* GDK_BUTTON_PRIMARY or _SECONDARY */
  uint32_t stroke_color; /* foreground for left button, background for right */
  uint32_t picked;       /* Pick Color: colour under the pointer */
  double   start_x, start_y; /* widget coordinates where the drag began */
  int      last_x, last_y;   /* last image pixel handled */
  int      start_ix, start_iy; /* image pixel where the drag began */
  uint32_t *backup;          /* pixels before a shape drag: lets the live preview be redrawn */
  EaselRect shape_prev;      /* area the previewed shape currently covers */

  /* Multi-step tools (polygon, curve): points collected so far. Polygon:
   * vertices. Curve: [0] start, [1] end, [2] and [3] control points. */
  gboolean   seq_active;
  int        seq_stage;     /* curve: 0 drawing the line, 1 awaiting bend, 2 awaiting 2nd bend */
  int        seq_n;
  EaselPoint seq[MAX_SEQ];
  gboolean   seq_moved;
  gint64     last_press_us;

  /* Resize handles: 0 none, 1 right edge, 2 bottom edge, 3 corner */
  gboolean show_grid;
  int      handle_hover;
  int      resize_handle;   /* handle being dragged, 0 when not resizing */
  int      resize_w, resize_h;

  /* Selection. The picture is only touched when the selection is first
   * moved ("lifted": its pixels go to sel_clip and a background-coloured
   * hole is left) and when it is committed (the clip is drawn back down). */
  gboolean    sel_active;
  EaselRect   sel_rect;      /* current position and size, image pixels */
  EaselClip  *sel_clip;      /* lifted contents, NULL until the first move */
  uint32_t   *sel_base;      /* picture before the lift, for undo */
  EaselRect   sel_edit;      /* area touched by the lift */
  GdkTexture *sel_texture;   /* sel_clip as shown on screen */
  gboolean    sel_creating, sel_moving;
  int         sel_grab_x, sel_grab_y;
  uint8_t    *sel_mask;      /* free-form shape inside sel_rect, NULL for a rectangle */
  GArray     *sel_path;      /* free-form outline (EaselPoint), NULL for a rectangle */
  int         sel_path_x, sel_path_y; /* sel_rect origin when the outline was made */
  EaselClip  *sel_src;       /* contents when a stretch began */
  EaselRect   sel_src_rect;
  int         sel_stretch;   /* handle being dragged (1..8), 0 none */
  /* Text tool */
  gboolean    text_active, text_creating;
  EaselRect   text_rect;
  GString    *text_buf;
  int         text_cursor;   /* byte offset */
  char       *font;
  EaselClip  *text_clip;
  GdkTexture *text_texture;
  PangoLayout *text_layout;
  gboolean    text_opaque, text_smooth;
  gboolean    panning;
  int         gradient_style;
  EaselRect   grad_region;   /* what the gradient fills: the picture or the selection */
  gboolean    grad_masked;
  gboolean    grad_pending;  /* drawn but still adjustable: drag its end points */
  int         grad_x0, grad_y0, grad_x1, grad_y1;
  int         grad_drag;     /* end point being dragged: 1 start, 2 end, 0 none */
  guint       grad_button;
  double      pan_x0, pan_y0, pan_h0, pan_v0; /* pointer and scroll position at the start */
  GArray     *text_attr;     /* TextAttr per byte of text_buf */
  int         text_anchor;   /* other end of the text selection, -1 none */
  guint       text_typing;   /* style flags for newly typed text */
  guint       text_blink_id;
  gboolean    text_caret_on;
  int         text_drag;      /* TEXT_DRAG_* */
  int         text_handle;    /* handle (1..8) while resizing */
  int         text_grab_x, text_grab_y;
  EaselRect   text_orig;
  GtkWidget  *ctx_popover[2];  /* right-click menus: selection, text */
  GMenuModel *ctx_model[2];
  int         ctx_pending;     /* menu to open when the button is released; -1 none */
  double      ctx_x, ctx_y;
  gboolean    eraser_opaque; /* Eraser paints the background colour; else transparency */
  gboolean    sel_opaque;    /* Draw Opaque: background-coloured pixels are kept */

  /* Undo */
  EaselHistory *history;
  EaselRect     edit_rect;  /* pixels touched by the edit in progress */
  uint32_t     *edit_base;  /* picture before a freehand edit (shape tools use `backup`) */

  /* Airbrush */
  guint    spray_source;
  uint32_t spray_rng;

  /* Magnifier */
  int    zoom_level;      /* zoom a click applies */
  gboolean center_pending;
  double center_x, center_y; /* image point to bring to the middle of the view */

  int hover_x, hover_y; /* pointer in image pixels, -1 when outside */
};

typedef struct { guint8 flags; uint32_t color; } TextAttr;

enum { SIGNAL_POINTER_MOVED, SIGNAL_COLOR_PICKED, SIGNAL_HISTORY_CHANGED, SIGNAL_RESIZE_PREVIEW, SIGNAL_ZOOM_CHANGED, SIGNAL_SELECTION_CHANGED, SIGNAL_TEXT_STATE, N_SIGNALS };
extern guint canvas_signals[N_SIGNALS];

/* Functions shared between the canvas source files. */
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

/* Pointer input as plain values (see canvas.c). (x, y) are widget coordinates
 * in logical pixels; the offsets are relative to the press. */
void canvas_press (EaselCanvas *self, guint button, double x, double y, gboolean ctrl);
void canvas_drag (EaselCanvas *self, double offset_x, double offset_y, gboolean shift);
void canvas_release (EaselCanvas *self, double offset_x, double offset_y, gboolean shift);
