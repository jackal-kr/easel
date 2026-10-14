---
name: add-tool
description: Use when adding or changing a drawing tool in Easel (new tool, new tool option, new cursor). Lists every place a tool touches.
---

# Adding or changing a tool

A tool touches, in this order:
1. `src/ui/tool.h` — enum value (keep toolbox order) and, if needed, a flag in `EASEL_TOOL_*`.
2. `src/ui/tool.c` — table row: name, symbolic icon, abbreviation, status-bar hint, flags.
3. `data/icons` + `data/*.gresource.xml` + `tools/gen-icons.py` — the `easel-<tool>-symbolic` icon.
4. `src/core/raster.c/.h` — pixel logic (libc only) **with tests** in `tests/test_raster.c`.
5. `src/ui/canvas-shapes.c` (or the file that fits) — apply the tool for press/drag/release; route in `canvas.c` `canvas_press`.
6. `src/ui/toolbox.c` — options page (`options_page_for`, `build_*_options`) and getters; push to canvas in `window.c: sync_canvas_tool`.
7. `src/ui/cursors.c` — cursor art.
8. `data/io.github.jackal_kr.Easel.gschema.xml` + `ui/preferences.c` — only if the tool gets a persisted default.
9. `tests/test_canvas.c` — a press/drag/release test incl. undo/redo and the modified flag.

Then run the `verify-change` skill. Remember: edits must go through `mark_dirty`, and anything pending must be
committed by `easel_canvas_flush()`.
