/* Undo/redo. Each step stores only the rectangle of pixels it changed (the
 * "before" and "after" contents), so steps on a big image stay small. Pure
 * engine code: it never looks at how the change was made. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/document.h"

typedef struct _EaselHistory EaselHistory;

/* Oldest steps are dropped once the stored pixels exceed `max_bytes`. */
EaselHistory *easel_history_new (size_t max_bytes);
/* Lets old steps live in a temporary file in `dir` (NULL: $TMPDIR or /tmp)
 * once the steps held in memory pass `ram_limit` bytes. `max_bytes` from
 * easel_history_new then counts memory and disk together. */
void          easel_history_set_spill (EaselHistory *history, const char *dir, size_t ram_limit);
void          easel_history_free (EaselHistory *history);
void          easel_history_clear (EaselHistory *history);

/* Records a step. `doc` already holds the result; `before` is a full copy of
 * the pixels (same size as `doc`) from before the edit; `area` is where the
 * edit may have changed pixels. The area is trimmed to what really changed;
 * returns false (and records nothing) if nothing did. Drops any redo steps. */
bool easel_history_push (EaselHistory *history, const EaselDocument *doc,
                         const uint32_t *before, EaselRect area);

/* Records a change of picture size (or any whole-picture change). `before`
 * holds the old pixels (before_w x before_h); `doc` is already the result. */
bool easel_history_push_full (EaselHistory *history, const uint32_t *before,
                              int before_w, int before_h, const EaselDocument *doc);

/* Remembers the current state as the saved one; is_saved tells whether the
 * picture is back at it (e.g. after undoing everything done since saving). */
void easel_history_mark_saved (EaselHistory *history);
bool easel_history_is_saved (const EaselHistory *history);

bool easel_history_can_undo (const EaselHistory *history);
bool easel_history_can_redo (const EaselHistory *history);

/* Apply a step to `doc`; `*changed` receives the rectangle that was touched
 * (the whole picture when its size changed). */
bool easel_history_undo (EaselHistory *history, EaselDocument *doc, EaselRect *changed);
bool easel_history_redo (EaselHistory *history, EaselDocument *doc, EaselRect *changed);
