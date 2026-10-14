/* A full-size copy of a picture's pixels that is kept up to date by copying
 * only the rectangle that changed since the last update. Used for the undo
 * "before" image and for the pixel buffers handed to the display.
 *
 * Reference counted (atomically): the display may still hold a buffer after
 * its owner has let go of it. A buffer someone else holds is "busy" and must
 * not be updated. Pure engine code. */
#pragma once

#include <stdbool.h>

#include "core/document.h"

typedef struct _EaselSyncBuf EaselSyncBuf;

struct _EaselSyncBuf
{
  uint32_t *data;       /* NULL until the first update */
  int       width, height;
  EaselRect stale;      /* where `data` may differ from the picture */
  int       refs;
};

EaselSyncBuf *easel_syncbuf_new (void);
EaselSyncBuf *easel_syncbuf_ref (EaselSyncBuf *buf);
void          easel_syncbuf_unref (EaselSyncBuf *buf);

/* True while anyone besides the owner holds a reference. */
bool easel_syncbuf_busy (EaselSyncBuf *buf);

/* Tells the buffer the picture changed in `r`. */
void easel_syncbuf_mark_stale (EaselSyncBuf *buf, EaselRect r);

/* Makes the buffer equal to `doc`. The first call (or a change of picture
 * size) copies everything; later calls copy only the stale rectangle. */
bool easel_syncbuf_update (EaselSyncBuf *buf, const EaselDocument *doc);
