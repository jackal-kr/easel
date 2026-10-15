# Easel: how it works

Oct 14, 2026 · @Oleksiy

A guided tour of the Easel source for a C programmer who is new to GNOME: what the pieces are, how a click becomes pixels, and where to look in each file, sized to read in about four hours.

## How to read this in four hours

Read the parts in order, with the matching source file open beside the doc. Never read all the code line by line: the doc says which functions carry the idea and which are bulk you can skip.

| Part | What you get | Minutes |
| --- | --- | --- |
| 1. What we improved | The change list with the gain of each | 10 |
| 2. Tech stack | Every layer from the kernel to your code | 20 |
| 3. Architecture | The two-layer design and why | 15 |
| 4. Flow | A mouse click to pixels, then undo, paste, save | 25 |
| 5. Key structures | The ten structs that matter | 20 |
| 6. Libraries | GLib, GObject, GTK, Adwaita, Cairo, Pango, Pixbuf | 20 |
| 7. Code tour: core/ | Six small files, pure C | 35 |
| 8. Code tour: canvas | The big widget, split in five files | 40 |
| 9. Code tour: rest of ui/ | Window, toolbox, colour box, dialogs, files | 25 |
| 10. Build and packaging | Meson, resources, i18n, tests, Flatpak, deb | 15 |
| 11-12. Tips and glossary | Habits, traps, vocabulary | 15 |

That is 240 minutes. If you only have two hours, read parts 1 to 5 and the first half of part 8, then use parts 6, 7 and 9 as a reference when you open a file.

Three habits make the reading faster. First, keep a terminal in the repo and run `grep -n function_name src -r` whenever a name appears; the doc uses real function names. Second, read the header (`.h`) of a file before its `.c`; headers hold most of the explanations. Third, ignore anything about translations (`_("...")`) while you read; it is just a text lookup that returns the same string in English.

## 1. What we improved and what you gain

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
| Comments rewritten to explain why | everywhere | A new reader learns the design from the code itself |
| Help is an HTML page opened in the browser | `docs/help.html`, `tools/build-help.py`, `main.c` | Readable help on any desktop without a Markdown viewer |

The features added earlier today are in the same releases:

| Release | Change | What you gain |
| --- | --- | --- |
| 1.1.0 | Fit to Window, Grid dialog (cell size and axonometric grid), Stretch and Skew by percent or pixels | Fewer clicks to see the whole picture; precise grids and resizing |
| 1.1.1 | Right-click outside a selection with a select tool opens the Paste menu | You can paste after copying without switching tool |
| 1.1.2 | The speed, safety and Help changes in the first table | See above |

Since then the three big input functions in `canvas.c` became small dispatchers, with one press/drag/release handler per tool (a `CanvasToolInput` table, handlers live next to each tool's code). A picture of 64 MB or more now gives back its undo shadow and spare texture copy after three idle seconds. Undo steps beyond the newest 64 MB are written to a temporary file in the cache folder (`history.c`, `easel_history_set_spill`; the file is deleted at once and disappears with the process), so the history can be 2 GB by default without filling memory. While you draw, a huge picture still costs about four times its size; cutting that further needs tile-based undo and is a good project once you know the codebase.

## 2. Tech stack

A GNOME app is a normal C program that sits on a tall stack of libraries. Knowing the layers tells you where to look when something breaks.

| Layer | What it is | Used in Easel for |
| --- | --- | --- |
| Linux kernel + Wayland/X11 | The OS and the display protocol | Not touched directly; GTK talks to it |
| GLib | The C utility library under everything GNOME: strings, lists, the main loop, files, settings | `GString`, `GArray`, `GFile`, `GSettings`, timers (`g_timeout_add`) |
| GObject | A class system written in C, on top of GLib: types, inheritance, properties, signals | `EaselCanvas`, `EaselWindow`, `EaselToolbox` and `EaselColorBox` are GObject classes |
| GIO | Files, async I/O, actions and menus, part of GLib | Open/save, `GAction` for every menu item, `GMenuModel` for menus |
| GDK | GTK's window-system layer: textures, clipboard, events | `GdkTexture` for the picture, `GdkClipboard` for copy and paste |
| GSK | GTK's scene graph and renderer (GL, Vulkan or Cairo) | Everything drawn in `snapshot()` becomes GSK nodes |
| GTK 4 | The widget toolkit | Buttons, scrolling, popovers, gestures, drawing areas |
| libadwaita | GNOME's modern widgets and style on top of GTK | Header bar, alert dialogs, preferences rows, light/dark scheme |
| Cairo | A 2D drawing library on the CPU | Cursors, tool previews, thumbnail, resizing pasted images |
| Pango | Text layout and fonts | The Text tool's layout and rendering |
| gdk-pixbuf | Loads and saves image files | PNG, JPEG, BMP, TIFF, WebP, GIF import |
| gettext | Translation lookup | The `_("text")` calls; Polish and Ukrainian catalogs |

The build tools matter just as much as the libraries:

- **Meson + Ninja** build the project (`meson.build` files, `ninja -C build`). Meson is a nicer replacement for autotools and CMake.
- **pkg-config** (called by Meson) finds the right compiler flags for each library.
- **glib-compile-resources** packs the menus, CSS and icons into the executable (a GResource).
- **glib-compile-schemas** compiles the settings description (a GSettings schema).
- **Flatpak** and **dpkg** package the app for users. The Flatpak build uses the GNOME 50 runtime, which is a ready-made set of all the libraries above.

A good picture of the whole thing: your code calls GTK; GTK calls GDK and GSK; GSK calls the GPU driver (or Cairo); everything uses GLib. Easel keeps its own picture in plain memory and hands it to GTK as a texture, so most of the interesting logic (drawing, undo, selections) does not depend on GTK at all.

## 3. Architecture

Easel has two layers with a one-way dependency. `core/` is plain C that stores the picture and implements every drawing, undo and selection algorithm. `ui/` is GTK code that shows that picture and turns input into calls on `core/`. Nothing in `core/` includes GTK, so it is tested on its own (`tests/test_raster.c`) and could be reused in a command-line tool.

&#91;embedded content: layers: libraries, ui/, core/ · who calls whom\]

Read the picture top to bottom: the platform libraries support `ui/`, `ui/` calls `core/`, and `core/` calls none of the libraries. Inside `ui/` the window owns the toolbox, the canvas and the colour box, and they talk to it only through signals. Three design records in `docs/decisions/` explain the choices that shape this: a pure-C core (0001), premultiplied pixels (0002), and rectangle-based undo with a shadow copy (0003).

## 4. Flow: from a click to pixels

GTK programs are **event driven**. `main()` does not loop over your logic; it hands control to GTK, which waits for events (mouse, keys, timers) and calls your functions. Everything below is a chain of such callbacks.

### Start-up

1. `main()` in `src/main.c` sets up translations, creates an `AdwApplication` and connects three signals: `startup`, `activate`, `open`. Then `g_application_run()` takes over.
2. `on_startup` registers the app-wide actions (Help, Preferences, Quit...), all keyboard accelerators, the CSS and the icon search path.
3. `on_activate` (or `on_open` when a file was given on the command line) builds an `EaselWindow` (`ui/window.c`).
4. `easel_window_init` creates the document, the canvas, the toolbox, the colour box and the status bar, puts them in a layout, registers the window actions, and connects the signals between the parts.

### One pencil stroke (the core loop)

1. The canvas has a `GtkGestureDrag` (set up in `easel_canvas_init`). When the left button goes down, GTK calls `on_drag_begin`, which calls `canvas_press(self, button, x, y, ctrl)`.
2. `canvas_press` converts the widget position to a picture pixel (`to_image`, which divides by the zoom) and calls the tool's press handler. For Pencil it sets `drawing = TRUE`, calls `canvas_begin_edit()` (takes the "before" snapshot for undo) and stamps one dot with `canvas_apply_stroke()`.
3. `canvas_apply_stroke` (in `canvas-shapes.c`) calls the pure engine: `easel_raster_stroke()` in `core/raster.c`. This writes pixels straight into `doc->pixels` and returns the rectangle it touched.
4. `canvas_mark_dirty(rect)` records that rectangle in four places: the undo edit rectangle, the dirty region for the GPU texture, and the "stale" rectangles of the undo shadow and the texture buffers. Then it calls `gtk_widget_queue_draw`.
5. As the mouse moves, GTK calls `on_drag_update` then `canvas_drag`, which joins the last point to the new one with a line, so fast strokes have no gaps.
6. When the button goes up, `canvas_release` calls `canvas_finish_edit()`: it compares the "before" snapshot with the picture inside the edit rectangle, stores only the changed box as an undo step (`easel_history_push`), and emits `history-changed`. The window listens and enables or disables the Undo and Redo buttons.

### Getting those pixels on the screen

GTK redraws on the next frame by calling `easel_canvas_snapshot`:

1. `refresh_texture()` returns at once if nothing is dirty. Otherwise it builds a new `GdkTexture` from the pixels, telling GDK the previous texture and the dirty region, so the GPU uploads only that region.
2. The texture's memory is one of three recycled copies of the picture (`TEXTURE_BUFFERS`), refreshed by copying only the stale rectangles. The real `doc->pixels` is never handed to GTK, because we keep writing to it while GTK may still be rendering the last frame.
3. The snapshot draws the checkerboard, the texture (nearest-neighbour when zoomed in, so pixels stay square), and then overlays: brush preview, eraser outline, grid, selection, text box, gradient handles, resize handles.

### Undo and redo

`Ctrl+Z` triggers the `win.undo` action, which calls `easel_canvas_undo`. That asks `easel_history_undo` to copy the saved "before" pixels back into the picture and report the rectangle it changed; the canvas marks that rectangle dirty and the normal drawing path does the rest. Redo copies the "after" pixels back.

### Shape tools (line, rectangle, ellipse, gradient)

These need a live preview, so they cannot just paint and forget. On press the canvas keeps a full `backup` of the picture. On every drag step it restores the previous preview area from the backup, then draws the shape again with the new corner. On release the last drawing stays and becomes one undo step. Polygon and Curve use the same trick but over several clicks (`canvas_seq_render`).

### Selection

A selection starts as a plain rectangle. Only when you first move it is it "lifted": its pixels are copied to `sel_clip`, the hole is filled with the background colour (or transparency) and the clip floats over the picture as its own texture. Pressing outside, switching tool, saving, or `easel_canvas_flush()` "commits" it: the clip is blended back at its new position. The lift and the landing together are one undo step.

### Paste

`Ctrl+V` reads the clipboard asynchronously (`gdk_clipboard_read_texture_async`), converts the texture to an `EaselClip`, and calls `easel_canvas_paste_clip`. If the image is bigger than the picture, a handler set by the window shows a dialog: shrink the image, or grow the canvas. Then the clip becomes a floating selection in the top-left of the visible area.

### Menus, actions and shortcuts

Every menu item in `data/menus.ui` names an action such as `win.undo`. The window registers those actions in a table (`GActionEntry`) that maps the name to a C function. Keyboard shortcuts are attached to the same action names in `main.c` (`gtk_application_set_accels_for_action`). So a click, a menu choice and a shortcut all end in the same function.

### Open and save

Open shows a `GtkFileDialog`; `ui/fileio.c` decodes with gdk-pixbuf and converts to premultiplied ARGB; `easel_canvas_set_document` swaps it in and clears undo. Save first calls `easel_canvas_flush` (so floating text or a selection are really in the picture), converts back to straight alpha (or flattens onto white for JPEG), and writes through `g_file_replace` so a failed save never destroys the old file.

## 5. Key structures

Learn these eleven and the rest of the code reads easily. The first six live in `core/` and are plain C structs; the last five are in `ui/`.

| Struct | File | What it holds | Who owns it |
| --- | --- | --- | --- |
| `EaselDocument` | `core/document.h` | `width`, `height`, and `pixels`: one `uint32_t` per pixel, row after row, no padding | The canvas; freed in its `finalize` |
| `EaselRect` | `core/document.h` | `x, y, width, height`. Used everywhere to say "this part of the picture" (dirty areas, selections) | A value, passed by copy |
| `EaselBrush`, `EaselEraser` | `core/raster.h` | Shape, size and colour of a stamp; eraser mode | Built on the stack for each call |
| `EaselHistory` and its `Step` | `core/history.c` | A growing array of undo steps, a position `pos`, a memory budget, and a `saved` marker | The canvas |
| `EaselSyncBuf` | `core/syncbuf.h` | A reference-counted copy of the picture plus a "stale" rectangle that tells what is out of date | The canvas (one `shadow` and three `tex_bufs`); GTK also holds references while it renders |
| `EaselClip` | `core/clip.h` | A rectangle of pixels with an optional one-byte-per-pixel mask; the contents of a selection | The canvas (`sel_clip`, `text_clip`) or the clipboard code |
| `EaselCanvas` | `ui/canvas-private.h` | The whole widget state: document, texture, tool, colours, gesture, selection, text, gradient, undo | A GTK widget; GTK owns it through the parent window |
| `EaselToolInfo` | `ui/tool.h`, `tool.c` | A table row per tool: name, icon, short label, status-bar hint, flags (is it a selection tool? does it preview a shape?) | Static, read-only |
| `EaselToolbox` | `ui/toolbox.c` | The tool buttons and the options pages; remembers sizes, shapes, fill style | The window |
| `EaselColorBox` | `ui/colorbox.c` | The 28-colour palette, foreground and background, which cell was clicked | The window |
| `EaselWindow` | `ui/window.c` | Pointers to the parts above, the current `GFile`, the `modified` flag, the thumbnail and frame-time helpers | The application |

### The pixel format

A pixel is a `uint32_t` written `0xAARRGGBB`. The colour channels are **premultiplied**: red, green and blue have already been multiplied by alpha. Half-transparent pure red is `0x80800000`, not `0x80FF0000`. This is the format Cairo and GTK use natively, so the buffer goes to the screen with no conversion, and blending is one multiply-and-add per channel. The price is that you must remember it when you read colours back: to invert a pixel you compute `alpha - channel`, not `255 - channel`, and to save a PNG you divide by alpha again.

### How the canvas fields fit together

The canvas holds the one true picture, `doc`. Three other things are copies of it, kept current only where something changed:

- `shadow` is the picture as of the last finished edit; undo compares against it.
- `tex_bufs[3]` are the memory that GPU textures read from.
- `backup` is a full copy taken at the start of a shape drag so the preview can be redrawn.

Selections and text boxes are *not* in the picture while you edit them. They are separate clips drawn on top, and they only become pixels of `doc` when committed. That is why `easel_canvas_flush()` exists and why saving always calls it first.

### GObject in thirty seconds

The four `ui/` classes use a few GObject conventions that look odd to a C programmer:

- `G_DEFINE_FINAL_TYPE (EaselCanvas, easel_canvas, GTK_TYPE_WIDGET)` generates the type registration, `easel_canvas_get_type()`, the `EASEL_CANVAS()` cast macro and the `easel_canvas_parent_class` pointer.
- `easel_canvas_class_init` runs once per class: it overrides virtual functions (`measure`, `snapshot`, `dispose`) and declares signals.
- `easel_canvas_init` runs once per object, like a constructor. There is no separate constructor call; fields start zeroed.
- `dispose` drops references to other objects and may run more than once; `finalize` frees plain memory and runs exactly once. Easel releases textures and timers in `dispose` and the document in `finalize`.
- **Signals** are named events: the canvas emits `history-changed` or `pointer-moved`, the window connects a callback with `g_signal_connect`. This is how the canvas tells the window things without knowing the window exists.

## 6. Libraries and what Easel uses from each

### GLib: memory habits you will see everywhere

GLib's C idioms replace the manual `free()` chains you know:

- `g_autofree char *s = ...;` frees `s` when the variable goes out of scope (a GCC/Clang `cleanup` attribute).
- `g_autoptr (GFile) file = ...;` does the same for objects, calling the right unref function.
- `g_clear_pointer (&p, free_fn)` frees and sets the pointer to NULL, so a double free is harmless. `g_clear_object (&obj)` is the same for GObjects.
- `g_steal_pointer (&p)` hands ownership to someone else and sets `p` to NULL, so the scope-exit cleanup does not free it.
- `g_set_object (&field, obj)` replaces a stored reference, unreffing the old and reffing the new.

The **main loop** is GLib's event loop. `g_timeout_add (ms, func, data)` calls `func` every `ms` milliseconds until it returns `G_SOURCE_REMOVE`. Easel uses timers for the caret blink, the airbrush spray, the thumbnail refresh and the frame-time label. Always remove a timer in `dispose`, or it will fire on a freed object.

### GObject

Reference-counted objects. `g_object_ref` and `g_object_unref` change the count; when it hits zero the object is destroyed. Widgets are special: the parent owns them, so you usually do not unref a widget you added to a container.

### GIO: actions and menus

A `GAction` is a named command (`win.undo`, `app.quit`). A `GMenuModel` describes a menu declaratively (`data/menus.ui`) and refers to actions by name. Buttons and menu items can be *bound* to an action, which then enables, disables and checks them automatically. `set_action_enabled()` in `window.c` is how Easel greys out Undo.

### GTK 4

- **Widget tree**: windows contain boxes contain buttons. Easel's canvas is a custom widget (a subclass of `GtkWidget`), so it must implement its own `measure` (how big am I?) and `snapshot` (draw me).
- **Snapshot drawing**: you do not paint into a surface. You append *render nodes* to a `GtkSnapshot` (`gtk_snapshot_append_color`, `append_scaled_texture`, `append_stroke`, `push_clip`...). GTK keeps the node tree and renders it on the GPU. If nothing changed, GTK may reuse the last frame.
- **Event controllers**: small objects attached to a widget to receive input: `GtkGestureDrag` (press, move, release as one unit), `GtkEventControllerKey`, `GtkEventControllerMotion`, `GtkEventControllerScroll`.
- **Logical vs device pixels**: on a HiDPI screen one GTK pixel is 2 device pixels. The canvas divides by `gtk_widget_get_scale_factor` so one picture pixel stays one device pixel at 100%.
- **Async dialogs and clipboard**: `GtkFileDialog` and `gdk_clipboard_read_texture_async` return immediately and call you back with a `GAsyncResult`. State that must survive until the callback goes in a heap struct or a field, not on the stack.

### libadwaita

GNOME's design layer. Easel uses `AdwApplication` (follows the system light or dark style), `AdwApplicationWindow` and `AdwHeaderBar`, `AdwAlertDialog` for questions such as "save changes?", and `AdwPreferencesGroup`, `AdwSwitchRow`, `AdwComboRow` and `AdwSpinRow` for the settings and the Stretch and Grid dialogs. Two quirks we hit: `AdwSpinRow` refuses unit text such as "50%" unless `numeric` is turned off, and `AdwAlertDialog` cannot be made wider.

### Cairo and Pango

Cairo draws vector graphics into a CPU image surface. Easel uses it where an immediate-mode API is easier than render nodes: the tool cursors, the small previews on option buttons, the thumbnail, and smooth shrinking of a pasted picture. Pango lays out text (fonts, wrapping, bold, italic); the Text tool asks Pango for the layout and renders it into a clip with `pangocairo`.

### gdk-pixbuf

The image file loader. `fileio.c` loads any supported format into a `GdkPixbuf`, copies it to premultiplied ARGB, and for saving builds a pixbuf back. WebP needs the extra `webp-pixbuf-loader` package at runtime, which is why the Debian package only *recommends* it.

### gettext

`_("Text")` looks the string up in a `.mo` catalog for the user's language and returns the English text when none exists. `N_("Text")` only marks a string for extraction without translating it (used in static tables). The `po/` directory holds the template (`easel.pot`) and the Polish and Ukrainian translations.

## 7. Code tour, part 1: `src/core/`

The engine. About 1,700 lines of plain C with no GTK, GLib or Cairo, so it can be tested without a display (`tests/test_raster.c`). Read it in this order; each file builds on the one before. Rule of the house: every drawing function returns the rectangle it touched, so the UI redraws and records undo for only that area.

### `document.h` / `document.c` (about 150 lines)

The picture. `EaselDocument` is `width`, `height` and a `pixels` array. Read the header comment on the pixel format first.

- `easel_document_new/free/fill`: allocate, release, fill with one colour. Sizes above `EASEL_MAX_PIXELS` (2^28, 1 GiB) are refused so a typo cannot exhaust memory.
- `get_pixel/set_pixel`: bounds-checked; out of range reads give 0 and writes are ignored.
- `easel_rect_union` and `easel_rect_clip`: the two rectangle helpers used everywhere. Clip cuts a rectangle to the picture and reports whether anything is left.
- `easel_document_resize`: keeps the top-left part, fills the new area; `easel_document_replace`: swaps in new contents of any size (used by undo of a resize).

### `raster.h` / `raster.c` (about 600 lines)

Every drawing primitive, deliberately **not anti-aliased**, like Windows XP Paint. Skim it; the shapes are standard algorithms.

- `easel_brush_covers`: is pixel (i, j) of an n by n brush painted? The canvas uses it for the hover preview, so preview and result cannot differ. Circles use doubled integer coordinates to avoid floating point.
- `easel_raster_stamp` and `easel_raster_stroke`: one brush footprint, and a Bresenham line of stamps. `bresenham()` is the shared line walker; `walk_line()` adapts it to stamping.
- `easel_raster_erase` and `easel_raster_erase_stroke`: the same for the eraser, including the "colour eraser" mode of the right button.
- `easel_raster_flood_fill`: a scanline fill with an explicit heap stack (`SeedStack`). It fills a whole horizontal run, then pushes one seed per run above and below. Read this one; it is the best short example of the style.
- `easel_raster_line`, `_shape`, `_polygon`, `_polyline`, `_curve`: lines are stamps of a round brush; rectangles, rounded rectangles and ellipses test each pixel with `shape_contains`; the border is the outer shape minus the same shape shrunk by the line width; curves are a Bezier flattened into line segments.
- `scan_crossings` + `fill_polygon`: the even-odd polygon fill. For every pixel row it finds where polygon edges cross the row, sorts them, and fills between pairs.
- `easel_raster_spray`: the airbrush; random dots in a circle from a small xorshift generator whose state the caller keeps.
- `easel_raster_gradient`: linear, radial and reflected gradients, blended per channel (valid because pixels are premultiplied).
- `easel_raster_polygon_mask`: the same scanline fill, but writing a one-byte-per-pixel mask; it is what makes Free-Form Select work.

### `history.h` / `history.c` (about 250 lines)

Undo and redo. Read `history.h` and the `Step` comment in the `.c`.

- A `Step` stores the changed rectangle with its pixels before and after. A "full" step stores whole pictures, for operations that change the size.
- `easel_history_push(history, doc, before, area)`: you pass the area you *might* have changed; it scans it, finds the box of pixels that really differ and stores only that. If nothing differs it records nothing, so a click that changes no pixel is not an undo step.
- The steps live in an array; `pos` is how many are applied. Everything at `pos` and beyond is redo. A new edit chops the redo steps off.
- A byte budget (`max_bytes`, 512 MB by default from Preferences) drops the oldest steps, but never the newest.
- `saved` remembers `pos` at the last save so `easel_history_is_saved` can say whether the picture matches the file, even after undo and redo; it becomes -1 when that state was thrown away.

### `syncbuf.h` / `syncbuf.c` (about 80 lines)

A full copy of the picture kept up to date cheaply. `mark_stale(rect)` grows a bounding box of what is out of date; `update(doc)` copies only that box (or everything the first time or after a resize). It is reference counted with atomics, and `busy` means "someone else, such as GTK, still holds it". The canvas uses it for the undo shadow and for the texture memory.

### `clip.h` / `clip.c` (about 150 lines)

A rectangle of pixels plus an optional mask: what you hold when a selection is lifted. `easel_clip_copy_from` copies it out of the picture, `easel_clip_erase` leaves the hole, `easel_clip_blit` puts it down (skipping masked-out pixels, and in transparent mode skipping the background colour), `easel_clip_scaled` stretches it with nearest-neighbour.

### `transform.h` / `transform.c` (about 150 lines)

Flip, rotate, scale, skew, invert, black and white. The first four work on any cell size (4-byte pixels or 1-byte masks) through the `copy_cell()` helper, so one routine transforms a selection's pixels and its mask the same way. Invert and black-and-white are written for premultiplied pixels.

## 8. Code tour, part 2: the canvas

The canvas is the heart of the app: about 3,700 lines in five files, all working on one struct. It is long because Paint has many tools, not because any one idea is hard. Read `canvas.h` (the public API) and the struct in `canvas-private.h` first, then the files below.

### `canvas.h` and `canvas-private.h`

`canvas.h` is what the window sees: setters (`set_tool`, `set_foreground`, `set_zoom`), actions (`undo`, `cut`, `paste`, `turn`), the signal list, and the oversize-paste handler type. `canvas-private.h` defines `struct _EaselCanvas` (about 120 fields, grouped by feature with a comment per group) and the functions shared between the canvas files. If a field name confuses you, search this file; every field has a comment.

### `canvas.c` (about 1,700 lines): read it in this order

| Region | What is in it | Read it? |
| --- | --- | --- |
| top to `refresh_texture` | `canvas_logical_per_pixel`, snapshots for undo (`canvas_snapshot_take/drop`), `canvas_mark_dirty`, the texture pipeline (`texture_bytes`, `build_texture`, `refresh_texture`) | Yes, closely: this is how pixels reach the screen |
| `measure` to `draw_grid` | Widget size, the brush and eraser hover previews, resize handles, the grid | Skim; drawing code |
| `easel_canvas_snapshot` | Builds the frame: checkerboard, shadow, texture, overlays | Yes |
| `canvas_push_undo`, `begin_edit`, `finish_edit` | The undo bookkeeping around one edit | Yes, short |
| `canvas_press` | One big `switch (tool)` that starts a gesture | Read the right-click part and two or three tools |
| `canvas_drag` | Continues the gesture: resize handles, pan, selection, shapes, strokes | Skim |
| `canvas_release` | Ends it and commits an undo step | Read the end |
| `on_drag_*`, `on_key_pressed`, `on_motion` | Translate GTK events into the three functions above; Escape, Enter and Delete handling; cursors | Yes, short |
| `class_init`, `init`, `dispose` | GObject plumbing: signals, gestures, cleanup | Read `init` and `dispose` |
| the rest | The public API: zoom, undo/redo, set\_document, resize, grid, fit to window | Reference |

Two ideas explain most of the file. **Dirty tracking**: every pixel change goes through `canvas_mark_dirty`, which is the only way the screen learns of it. **The gesture state machine**: `drawing`, `button`, `start_x/y`, `last_x/y`, `sel_*`, `seq_*` and `grad_*` fields decide what `drag` and `release` mean for the current tool.

### `canvas-shapes.c` (about 280 lines)

`canvas_apply_stroke` (pencil, brush, eraser along a segment); `canvas_update_shape` (preview for line, rectangle, ellipse, rounded rectangle: restore from backup, draw again); `canvas_seq_render` and `canvas_commit_seq` (polygon and curve across several clicks); `canvas_spray_tick` (the airbrush timer); `canvas_grad_render`, `canvas_grad_commit` and `canvas_draw_gradient_guide` (a gradient stays adjustable until accepted).

### `canvas-selection.c` (about 540 lines)

The selection lifecycle (`canvas_sel_lift`, `canvas_sel_commit`, `canvas_sel_discard`), the eight stretch handles, drawing the floating clip and its dashed outline, and the clipboard: `copy`, `cut`, `paste`, `paste_clip_as` (with `clip_fit`, a Cairo shrink), and `on_clipboard_files` for files copied in the file manager. `easel_canvas_flush` lives here; it is the "make everything real" call. Read the comments on `canvas_sel_lift` and `canvas_sel_commit` first.

### `canvas-text.c` (about 700 lines)

The Text tool, effectively a small text editor. State: `text_buf` (a `GString`), `text_cursor` (a byte offset), `text_anchor` (the other end of a selection), `text_attr` (style per byte). `canvas_text_render` lays the text out with Pango into `text_clip`; `canvas_draw_text` shows it with caret and selection highlight; `canvas_text_key` handles typing, arrows, Home, End, Backspace, Delete and Ctrl shortcuts; `canvas_text_commit` blends the result into the picture with ordinary source-over alpha blending. Skim unless you plan to change text behaviour.

### `canvas-transform.c` (about 240 lines)

The Image menu. A small `Transform` struct (stretch, skew, flips, quarter turns, invert) is applied in a fixed order by `run_transform`, to the pixels and, in the same way, to the selection mask. `apply_transform` chooses between the selection and the whole picture and records one undo step. Short and readable.

### `cursors.c`, `tool.c`

`cursors.c` draws every mouse cursor with Cairo at start-up (crosshair, pencil, bucket, dropper, magnifier, airbrush) so no image files are needed. `tool.c` is the table of tools: name, icon, two-letter fallback label, status-bar hint and flags. Adding a tool means adding a row, an enum value in `tool.h`, an icon, and a `case` in `canvas_press`.

## 9. Code tour, part 3: the rest of `src/`

This is the "ordinary GNOME app" half. It is the part to study if you want to learn how GTK programs are put together, since it uses actions, menus, dialogs and settings the way most GNOME apps do.

| File | Lines | What it does | Concepts to notice |
| --- | --- | --- | --- |
| `main.c` | 230 | Creates the `AdwApplication`, sets up translations, registers app actions and **all keyboard shortcuts**, loads CSS and the icon path | `startup` / `activate` / `open` signals; `gtk_application_set_accels_for_action` |
| `ui/window.c` | 1,340 | The main window: builds the layout, owns the actions (New, Open, Save, zoom, grid, clipboard, image menu), the unsaved-changes flow, the thumbnail window, the frame-time label, the status bar | `GActionEntry` table, async callbacks, `confirm_discard` with a continuation function |
| `ui/toolbox.c` | 1,020 | The tool buttons and, below them, a `GtkStack` of options pages that follows the selected tool. Redoes its own layout between two and three columns on short windows | Custom layout, radio-button groups, `GtkDrawingArea` previews that call the real `easel_brush_covers` |
| `ui/colorbox.c` | 540 | The 28-colour palette, the two overlapping swatches, double-click or F2 to edit a colour, keyboard navigation, screen-reader names | Drawing with Cairo, `GtkColorDialog`, keeping an edited palette in settings |
| `ui/dialogs.c` | 470 | Image Attributes, Stretch and Skew, Grid, plus the `unit_spin_row` helper that shows "50%" or "64 px" in a spin row | `AdwAlertDialog` with custom content, two ways of editing one value (percent and pixels) kept in sync |
| `ui/fileio.c` | 140 | Load and save through gdk-pixbuf; converts between straight and premultiplied alpha; flattens onto white for JPEG and BMP | Atomic save with `g_file_replace` |
| `ui/print.c` | 70 | Prints through `GtkPrintOperation`, scaled to fit the page at 96 dpi | Print callbacks |
| `ui/preferences.c` | 310 | The Preferences window, one row per setting bound to GSettings | `AdwPreferencesGroup` rows, `g_settings_bind` |
| `ui/settings.c` | 150 | `easel_settings()` finds the compiled schema, even when run from the build folder; helper getters `easel_pref_int/bool/string`; applies the colour scheme | `GSettingsSchemaSource` |
| `ui/config.h` | 56 | Constants in one place: zoom steps, timer intervals, limits, default font, colour helpers |  |
| `ui/tool.c`, `cursors.c` | 68, 227 | See part 8 |  |

### How the window wires the parts together

In `easel_window_init` the window creates the toolbox, the canvas and the colour box, and then connects them with signals. This is the whole architecture of the UI in six lines of thought:

1. Toolbox `tool-changed` / `options-changed`: the window copies the tool and its options to the canvas (`sync_canvas_tool`).
2. Colour box `colors-changed`: the window copies foreground and background to the canvas.
3. Canvas `pointer-moved`: the status bar shows the coordinates.
4. Canvas `history-changed`: Undo and Redo buttons are enabled or disabled, and the title shows "modified".
5. Canvas `color-picked`: the colour box takes the colour and the previous tool comes back.
6. Canvas `selection-changed` and `text-state`: Cut, Copy, Delete and the Bold/Italic buttons are enabled or disabled.

The parts never call each other directly; they only emit signals, and the window decides what to do. That is why the canvas can be tested without a window.

### Why `confirm_discard` looks strange

Asking "Save changes?" is an *asynchronous* dialog: the function returns immediately and your code resumes later in a callback. So `confirm_discard (self, next)` receives the follow-up as a function pointer `next` and calls it once the user has answered. Opening a file from the command line that has to wait for that answer parks the file in the static `pending_open_file` until the callback runs. This continuation style is how you do "do X, but first ask the user" in any GTK 4 app.

### Settings in three steps

1. `data/io.github.jackal_kr.Easel.gschema.xml` declares each key with a type and a default.
2. `settings.c` returns the `GSettings` object, and `easel_pref_int ("line-width", 1)` reads a key.
3. `preferences.c` shows a row per key and writes changes back. The schema must be compiled (`glib-compile-schemas`) for GSettings to find it; Meson does this at build time and again at install.

## 10. Build, data files, packaging, translations, tests

### Everyday commands

```
meson setup build          # once: configure
ninja -C build             # compile (only what changed)
build/src/easel            # run it straight from the build folder
meson test -C build        # core tests, canvas test, desktop and metainfo validation
tools/check-i18n.sh        # every translatable file listed, no untranslated strings
```

The root `meson.build` states the dependencies (GTK 4.14, libadwaita 1.8, gdk-pixbuf), the compiler warnings and the `-D` defines that become C macros: `EASEL_VERSION`, `EASEL_APP_ID`, `GETTEXT_PACKAGE`, `LOCALEDIR`, `EASEL_DOCDIR`. `src/meson.build` builds `core/` as a static library (`easel-core`) and links it into the executable, which keeps the engine separate and lets `tests/` link only the engine.

### The `data/` folder

| File | What it is |
| --- | --- |
| `menus.ui` | All menus, written as XML and loaded as `GMenuModel`s; items name `win.*` or `app.*` actions |
| `style.css` | A few CSS rules for the toolbox and colour box (GTK is themed with CSS) |
| `*.gresource.xml` | Lists files to embed in the executable: menus, CSS, tool icons. Reach them as `/io/github/jackal_kr/Easel/...` |
| `*.gschema.xml` | Declares the settings keys and defaults |
| `*.desktop.in` | The launcher entry (name, icon, which files it opens) |
| `*.metainfo.xml.in` | AppStream data: description, screenshots, release notes. App stores read this |
| `icons/` | The app icon and the tool icons (SVG) |

### Packaging and releases

- **Debian package**: `tools/build-deb.sh` runs `dpkg-buildpackage`; the recipe is in `debian/` (`control`, `rules`, `changelog`). The package version comes from `debian/changelog`.
- **Flatpak**: `tools/build-flatpak.sh [--install]` builds against the GNOME runtime. The manifest for Flathub (the public store) is `build-aux/flathub/io.github.jackal_kr.Easel.yml`; it points at a git tag. A Flatpak app is sandboxed, so file access goes through *portals*; that is why a missing documents portal once made the Flatpak crash at launch even though the app was fine.
- **A release** is: bump `version` in `meson.build`, add an entry to `debian/changelog` and `<release>` in the metainfo, commit, tag `vX.Y.Z`, push, then build the deb and Flatpak and attach them to a GitHub release.

### Translations

`po/` holds `easel.pot` (the template of every translatable string), `pl.po` and `uk.po`. `POTFILES.in` lists the source files to scan. After adding or changing a `_("...")` string run `ninja -C build easel-update-po`, translate the new entries, and run `tools/check-i18n.sh`. The app picks the language from the environment through `setlocale` and `bindtextdomain` in `main()`.

### Accessibility

Screen readers read widgets through the accessibility tree. Easel gives the canvas, swatches and header buttons labels with `gtk_accessible_update_property`, and uses `gtk_accessible_announce` for events with no widget ("Foreground colour set to Red"). Pure GTK widgets already expose the rest. Orca (the GNOME screen reader) has not been tested with it yet.

### Tests

- `tests/test_raster.c` (766 lines, no display needed) covers the engine: brushes, fills, shapes, history, clips, transforms, sync buffers.
- `tests/test_canvas.c` drives the real canvas by calling `canvas_press`, `canvas_drag` and `canvas_release` directly, then checks pixels and undo. It needs a display, so it skips (exit code 77) without one. A randomised test does hundreds of edits and undos and checks the picture equals the expected state after each step. To run it headless: `GSK_RENDERER=cairo meson test -C build` inside a Wayland or X session.

### Developer aid

`EASEL_SCREENSHOT=out.png build/src/easel` saves a screenshot of the window and quits. It was used for the store screenshots.

### The `docs/` folder

`architecture.md` is the short version of this guide. `decisions/` has four numbered design records (pure-C core, premultiplied pixels, rectangle undo with a shadow, GSettings preferences): read them for the *why*. `specs/` has a numbered specification for each feature. `help.md` is the user manual, turned into `help.html` by `tools/build-help.py`.

## 11. Tips and tricks

### Debugging a GTK app

- **GTK Inspector**: run with `GTK_DEBUG=interactive` or press `Ctrl+Shift+I` in a running app. It shows the widget tree, CSS, properties and render nodes. Most "why is this widget the wrong size" questions are answered in a minute here.
- **Messages**: `G_MESSAGES_DEBUG=all` shows debug output; `G_DEBUG=fatal-warnings` makes the first `g_warning` abort, so gdb stops at the cause. GTK warnings are rarely harmless; fix them.
- **Renderer**: `GSK_RENDERER=cairo` forces software rendering. If a bug disappears with Cairo, suspect the GPU path; this is also how the tests run.
- **Memory bugs**: `meson setup build-asan -Db_sanitize=address,undefined`, then run the tests. Run the canvas test with `ASAN_OPTIONS=detect_leaks=0` first; GTK and fontconfig report harmless leaks at exit.
- **Speed**: View then Frame Time shows how long the last snapshot took (`paint_us`). For real profiling use Sysprof.

### Habits that prevent most bugs in this codebase

1. **After changing pixels, call `canvas_mark_dirty`.** Forget it and the picture changes but the screen does not, and undo may miss the change.
2. **Call `easel_canvas_flush` before anything that reads the whole picture** (save, print, transforms, resize). Floating selections and text are not in `doc` yet.
3. **Remember premultiplied alpha.** Never write `255 - channel`; never compare a colour you read back with one from a colour dialog without converting.
4. **Remove timers and handlers in `dispose`.** A timer that outlives its widget calls freed memory.
5. **Take a reference before an async call and drop it in the callback** (`g_object_ref (self)` / `g_object_unref (self)`, as the paste code does). The user may close the window while the clipboard read is pending.
6. **Keep `core/` free of GTK.** It is what makes the fast tests possible. If you need a GLib function there, you probably want the code in `ui/` instead.
7. **Return the touched rectangle** from any new drawing function.

### Recipes

**Add a menu command.** Add an `<item>` with the label and `win.my-action` in `data/menus.ui`. Add `{ "my-action", action_my_action }` to the action table in `window.c` and write the function. Optionally add a shortcut in `main.c`. Add the label to `po/POTFILES.in` if the file is new, and translate it.

**Add a tool.** Add an enum value in `ui/tool.h`, a row in `tool.c`, an icon (SVG, listed in the gresource file), a `case` in `canvas_press`, `canvas_drag` and `canvas_release`, and an options page in `toolbox.c` if it has settings.

**Add a setting.** Add a `<key>` to the gschema, read it with `easel_pref_*` where used, and add a row in `preferences.c`.

**Add a drawing primitive.** Write it in `core/raster.c` returning the touched `EaselRect`, declare it in `raster.h`, and add a test in `tests/test_raster.c` first; the engine is the easiest place to be correct.

### Reading unfamiliar GNOME code

Start from the signals and the action table, not from `main()`. Search for `g_signal_connect` to see who reacts to what, and for the action name from the menu (`grep -rn "zoom-fit" src data`) to find the one function behind a menu item. GNOME's own docs live at `docs.gtk.org` (GTK, GLib, GObject, GIO) and `gnome.pages.gitlab.gnome.org/libadwaita` (Adwaita); the GNOME Human Interface Guidelines at `developer.gnome.org/hig` explain *why* GNOME apps look the way they do.

## 12. More: glossary and what to read next

### Glossary

| Term | Meaning |
| --- | --- |
| Premultiplied alpha | Colour channels already multiplied by alpha. The native format of Cairo and GTK |
| Texture (`GdkTexture`) | An image living in GPU-friendly form, drawn by the renderer |
| Snapshot (`GtkSnapshot`) | The list of render nodes a widget produces each frame; also the name of the draw callback |
| Render node (GSK) | One drawing instruction: colour, texture, clip, shadow, gradient, stroke |
| Dirty region | The part of the picture changed since the last upload to the GPU |
| Stale rectangle | The part of a copy that no longer matches the picture |
| Shadow | Our name for the full copy of the picture as of the last finished edit; the undo base |
| Clip (`EaselClip`) | Pixels plus optional mask lifted out of the picture: a selection or text |
| Lift and commit | Taking a selection off the picture to float, and putting it back down |
| Action (`GAction`) | A named command such as `win.undo`; menus, buttons and shortcuts trigger it |
| Accelerator | A keyboard shortcut attached to an action name |
| Signal | A named event on a GObject; you connect callbacks to it |
| `dispose` / `finalize` | Two-stage object destruction: release references, then free memory |
| GResource | Files compiled into the executable (menus, CSS, icons) |
| GSettings / schema | Per-user persistent settings and the XML describing their keys |
| Portal | A service that lets a sandboxed Flatpak app use files, printing and more |
| Flathub | The public Flatpak app store |
| Metainfo / AppStream | The XML that describes the app to software stores |

### A suggested order for hands-on learning

1. Build and run it. Press `Ctrl+Shift+I` and click around the widget tree.
2. Draw one line with a debugger breakpoint in `canvas_press`; step to `easel_raster_stroke`, then to `canvas_mark_dirty`, then to `easel_canvas_snapshot`. This one trace teaches the whole architecture.
3. Make a small change in `core/raster.c` (for example, make the SLASH brush thicker), run `meson test`, and see the test catch it.
4. Add a trivial menu command (a recipe in part 11). This teaches actions, menus and signals.
5. Read the four design records in `docs/decisions/`. They are one page each.

### Where to read more

- GTK 4 documentation and the Getting Started tutorial: `docs.gtk.org/gtk4`
- GLib and GObject tutorial: `docs.gtk.org/gobject`
- libadwaita: `gnome.pages.gitlab.gnome.org/libadwaita`
- GNOME Human Interface Guidelines: `developer.gnome.org/hig`
- Meson manual: `mesonbuild.com`
- Flatpak docs: `docs.flatpak.org`; Flathub submission rules: `docs.flathub.org`
