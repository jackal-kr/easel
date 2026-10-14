# Easel

Native GNOME paint app: Windows XP Paint's functionality and workflow, built only from GNOME
components (GTK 4 + libadwaita), C, Meson. Not an XP visual clone. App ID
`io.github.jackal_kr.Easel`, GPL-3.0-or-later. Owner: JOD (prefers direct, concise answers, small
incremental steps, no token waste).

## Commands
    meson setup build                      # once
    ninja -C build                         # must stay warning-free
    meson test -C build --print-errorlogs  # core tests always; canvas tests need a display (skip, exit 77, otherwise)
    ./build/src/easel                      # run (GSettings schema is found in build/data)
Canvas tests headless-safe: `GSK_RENDERER=cairo meson test -C build`.
Env: Ubuntu 26.04, GTK 4.22, libadwaita 1.9 (AdwShortcutsDialog needs >= 1.8), GCC 15, Meson 1.10.

## How we work (engineering mode)
1. **Spec first** for anything non-trivial: `docs/specs/NNNN-title.md` from `_template.md` (use the
   `spec-first` skill). Behavior, acceptance criteria, test plan, out of scope. Get it agreed, then build.
2. **Small slices**, each leaving build + tests green. Use the `verify-change` skill before saying "done".
3. **Tests with the change**: core logic in `tests/test_raster.c`; widget-level behavior through
   `canvas_press/drag/release` in `tests/test_canvas.c`. A bug fix starts with a failing test when feasible.
4. **Decisions** that a future reader would question go in `docs/decisions/NNNN-*.md` (short ADRs).
5. **Git**: never commit or push unless asked. Commit messages: imperative summary, body explains why.
   One logical change per commit. Don't commit `build/` or `.scratch/`.
6. Update `docs/roadmap.md` when scope or status changes; keep this file for rules, not status.

## Architecture (details: docs/architecture.md)
- `src/core/` pure C, libc only (no GTK/GLib/Cairo): document, raster, history, syncbuf, transform,
  clip. Everything here has tests in `tests/test_raster.c`.
- `src/ui/` GTK/libadwaita. UI turns input into core calls; drawing logic never lives in widgets.
  Canvas is split: `canvas.c` (widget, render, input), `canvas-selection.c`, `canvas-text.c`,
  `canvas-shapes.c`, `canvas-transform.c`; shared internals in `canvas-private.h` (`canvas_` prefix).
- Tools are defined once in `ui/tool.c` (name, icon, hint, flags). Shared constants/colors: `ui/config.h`.
- Pixels `uint32_t` 0xAARRGGBB premultiplied. Hard-edged by default (no antialiasing), 1 image px = 1 device px at 100%.
- Undo: rect-based history with a "saved" marker; `EaselSyncBuf` keeps a shadow copy as undo base.
- Settings: GSettings schema `io.github.jackal_kr.Easel` (`data/*.gschema.xml`); read via `ui/settings.h`.
- Keep it simple: no threads/caches/fancy rendering until a measurement justifies them.

## Conventions
- GNU-ish C style, 2-space indent, space before `(`, return type on its own line, `//` avoided
  in favor of `/* */`. Match the surrounding file; do not reformat untouched code.
- Warnings are bugs (`-Wshadow -Wmissing-prototypes` etc. are on). Non-static functions need prototypes.
- Public UI API prefix `easel_<module>_`; canvas-internal shared functions `canvas_`.
- Menus: `data/menus.ui` GMenus; actions registered in `window.c`; absent action = disabled item.
- User-visible strings are English now; i18n is a planned packaging step (see roadmap) — keep strings in
  one place per dialog, don't build sentences by concatenation.
- No XP limits: undo is bounded only by a memory budget (Preferences, default 512 MB).

## Behavior reference
Behavior studied from jspaint (MIT, https://github.com/1j01/jspaint); we write our own C, no code
copied; credit stays in About. Hidden XP behaviors to keep: Ctrl = third color slot; Shift-drag smears a
selection, Shift-click stamps; numpad +/- resizes brush; Ctrl+G grid at 4x+; second mouse button cancels a
stroke; pasting a larger image expands the canvas; rounded-rect radius capped at 8; airbrush 5 ms timer.

## Gotchas learned
- Anything pending (floating selection, text being typed, unfinished curve) reaches the history only when
  put down. Call `easel_canvas_flush()` before asking "is it modified?" or saving/closing.
- The canvas takes keyboard focus; the viewport has scroll-to-focus off, or the view jumps to the corner.
- `EaselSyncBuf` copies only the stale rect; any new code that writes pixels must go through `mark_dirty`.
- Right button draws with the background color; right-click on a selection/text box opens a context menu instead.
