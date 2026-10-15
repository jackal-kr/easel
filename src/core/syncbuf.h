/* A full copy of the picture that we keep in step with the real one by
 * copying only the rectangles that changed. Copying a 4000x3000 picture on
 * every pencil stroke would be painfully slow; this makes it a few hundred
 * bytes instead.
 *
 * Used twice: as the "before" image for undo, and as the pixel memory behind
 * the GPU texture.
 *
 * The reference count is atomic because GTK may render on another thread and
 * keep a buffer alive after we are done with it. While somebody else still
 * holds it the buffer is "busy" and must not be touched, so the caller picks
 * another one (or makes a plain copy). */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>

#include "core/document.h"

typedef struct _EaselSyncBuf EaselSyncBuf;

struct _EaselSyncBuf
{
  uint32_t *data;       /* NULL until the first update */
  int       width, height;
  EaselRect stale;      /* bounding box of everything that changed since the last update */
  _Atomic int refs;
};

EaselSyncBuf *easel_syncbuf_new (void);
EaselSyncBuf *easel_syncbuf_ref (EaselSyncBuf *buf);
void          easel_syncbuf_unref (EaselSyncBuf *buf);

/* True while anyone besides the owner holds a reference. */
bool easel_syncbuf_busy (EaselSyncBuf *buf);

/* Gives the pixel memory back to the system if nobody else holds the buffer.
 * The next easel_syncbuf_update() makes a full copy again. For big pictures
 * that sit unused, so the spare copies do not cost memory for nothing. */
void easel_syncbuf_trim (EaselSyncBuf *buf);

/* Tells the buffer the picture changed in `r`. */
void easel_syncbuf_mark_stale (EaselSyncBuf *buf, EaselRect r);

/* Makes the buffer equal to `doc`. The first call (or a change of picture
 * size) copies everything; later calls copy only the stale rectangle. */
bool easel_syncbuf_update (EaselSyncBuf *buf, const EaselDocument *doc);
