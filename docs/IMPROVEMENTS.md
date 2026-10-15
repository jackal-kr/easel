# What we improved and what you gain

The last pass made the code smaller, a little faster and safer, and easier to read; it added no features. None of the speed gains were benchmarked: they remove obvious waste (per-pixel function calls, whole-picture copies), so treat the gain column as "what it saves", not as measured numbers.

| Change | Where | What you gain |
| --- | --- | --- |
| One `easel_rect_clip()` replaces four copies of the same clipping code | `core/document.c`, used by history, syncbuf, clip, raster | Less code to get wrong; one place to fix a bug in rectangle clipping |
| One `EASEL_MAX_PIXELS` constant instead of a magic `1 << 28` in two files | `core/document.h`, `clip.c` | The size limit cannot drift apart between files |
| Resize and paste copy whole rows with `memcpy` | `easel_document_resize`, `easel_document_replace` | Large pictures resize and paste faster (memcpy is vectorised, a per-pixel loop is not) |
| Flip, rotate, scale, skew copy 4-byte pixels and 1-byte masks on a fast path | `copy_cell()` in `core/transform.c` | No function call per pixel; an Image menu operation on a big picture is quicker |
| One Bresenham line walker used by pencil, eraser, shapes and free-form selection | `bresenham()` in `core/raster.c` | Three copies of the same algorithm became one |
| One scanline routine for polygon fill and for the free-form selection mask | `scan_crossings()` in `core/raster.c` | The fill and the selection can no longer disagree about what is "inside" |
| Committing text uses the shared snapshot instead of copying the picture | `canvas_text_commit()` in `canvas-text.c` | Clicking away from a text box on a big picture no longer copies tens of MB |
| Reference count declared `_Atomic int` instead of casting a plain `int` | `core/syncbuf.h` | Removes undefined behaviour in the buffers GTK renders from on another thread |
| Canvas struct regrouped by feature with a comment per group | `canvas-private.h` | You can find the selection, text or gradient state at a glance |
| Comments rewritten to explain why, added | everywhere | A new reader learns the design from the code itself |
| Help is an HTML page opened in the browser | `docs/help.html`, `tools/build-help.py`, `main.c` | Readable help on any desktop without a Markdown viewer |

The feature releases around it:

| Release | Change | What you gain |
| --- | --- | --- |
| 1.1.0 | Fit to Window, Grid dialog (cell size and axonometric grid), Stretch and Skew by percent or pixels | Fewer clicks to see the whole picture; precise grids and resizing |
| 1.1.1 | Right-click outside a selection with a select tool opens the Paste menu | You can paste after copying without switching tool |
| 1.1.2 | The speed, safety and Help changes in the first table | See above |

The three big input functions in `canvas.c` are now small dispatchers with one handler per tool, the polygon/curve array moved off the canvas struct (4920 to 856 bytes), and a large idle picture gives back its undo and spare texture copies (about 4x down to 2x its size). Old undo steps (beyond the newest 64 MB) now live in a temporary file, so the default history grew from 512 MB to 2 GB. One thing is still open: while you draw on a huge picture it costs about four times its size. Fixing that needs tile-based undo (see `docs/HOW-IT-WORKS.md`).
