# Architecture

## Layers
    core (libc)  ->  ui (GTK4 + libadwaita)  ->  main.c / data (GSettings, menus, icons)

### core
| File | Role |
|---|---|
| document.c | pixel buffer (`uint32_t` premultiplied ARGB), resize/replace |
| raster.c | brush stamps, strokes, eraser, fill, shapes, gradient, spray |
| history.c | rect-based undo/redo, whole-image steps, memory budget, "saved" marker |
| syncbuf.c | refcounted full-size copy kept current by copying only stale rects |
| transform.c | flip/rotate/stretch/skew/invert/black-and-white |
| clip.c | selection pixels + mask |

### ui
| File | Role |
|---|---|
| canvas.c | widget, texture rendering, pointer/key/scroll input (`canvas_press/drag/release`), zoom, undo/redo, set_document |
| canvas-selection.c | select/free-form select, floating selection, clipboard, context menu API |
| canvas-text.c | text box editor (caret, selection, styles, render to clip) |
| canvas-shapes.c | freehand tools, line/curve/polygon/shapes, gradient, sequence commit |
| canvas-transform.c | Image-menu transforms on selection or picture |
| canvas-private.h | `struct _EaselCanvas`, shared internals |
| tool.c/.h | one table: name, icon, abbreviation, hint, flags (selection/shape/hover preview) |
| toolbox.c | tool buttons + per-tool options pages |
| colorbox.c | palette, fg/bg swatches, Edit Colors |
| window.c | header bar, actions, files, thumbnail, frame time, status bar |
| dialogs.c / print.c / preferences.c / settings.c | dialogs, printing, Preferences, GSettings access |
| fileio.c | load/save via gdk-pixbuf (png, jpeg, bmp, tiff, webp; gif open only) |
| config.h | shared constants and color helpers |

## Rendering
Edits happen on the CPU buffer. `mark_dirty` records the rect (undo edit rect, GDK dirty region, stale rects
of the shadow and texture buffers). A pool of 3 texture buffers (GBytes) feeds a GdkTexture drawn with a
nearest-neighbour scaled texture node.

## Undo
Before an edit starts the canvas takes the shadow (`canvas_snapshot_take`) as the "before" picture; on finish
`easel_history_push` diffs the edit rect and stores only the changed bounding box. Whole-image ops use
`push_full`. The history knows which step equals the saved file (`is_saved`).

## Input and tests
Gesture callbacks only translate to `canvas_press/drag/release(button, x, y, ctrl/shift)`, which tests call
directly. Canvas tests need a display (skipped otherwise) and run with `GSK_RENDERER=cairo`.
