# Easel

Native GNOME paint app: Windows XP Paint's functionality and workflow, built only from
GNOME components (GTK 4 + libadwaita). Not an XP visual clone. App ID `io.github.jackal_kr.Easel`,
license GPL-3.0-or-later. Owner goes by JOD; prefers direct, concise answers and small
incremental steps.

## Build / run / test
    meson setup build        # once
    ninja -C build
    ./build/src/easel
    meson test -C build --print-errorlogs
Environment: Ubuntu 26.04, GTK 4.22, libadwaita 1.9, Cairo 1.18, GCC 15, Meson 1.10.

## Architecture rules
- `src/core/` is pure C (libc only): document pixel buffer, rasterizer, later tools/selection/
  history/file I/O. It must never include GTK, GLib or Cairo. Everything in it gets a test in `tests/`.
- `src/ui/` is GTK/libadwaita: window, canvas widget, toolbox, colorbox. UI translates input into
  core calls; drawing logic never lives in widgets.
- Pixels: `uint32_t` 0xAARRGGBB premultiplied (matches Cairo ARGB32 and
  GDK_MEMORY_B8G8R8A8_PREMULTIPLIED on little-endian). One buffer per document.
- Display: GPU via GSK (`gtk_snapshot_append_scaled_texture`, nearest filter). Editing: CPU.
  One image pixel = one device pixel at zoom 1 (HiDPI aware).
- Hard-edged tools by default (no antialiasing): pencil/line/fill/ellipse match XP pixel behavior.
- No XP limits: undo is unbounded within a memory budget (default 256 MB), tile-based
  copy-on-write history (not built yet). Whole-image ops store one snapshot.
- Menus are one GMenu (`data/menus.ui`) with XP's items. Unimplemented actions are simply
  absent, so GTK shows them disabled; implement the action to enable the item.
- Keep it simple: no threads, caches or fancy rendering until a measurement justifies them.

## Scope: 16 XP tools
Free-Form Select, Select, Eraser (right button = color eraser), Fill, Pick Color, Magnifier,
Pencil, Brush, Airbrush, Text, Line, Curve, Rectangle, Polygon, Ellipse, Rounded Rectangle.
Menus: File, Edit, View, Image, Colors, Help. Cut: Send, Page Setup.

## Behavior reference (learned from jspaint, MIT, https://github.com/1j01/jspaint)
We study behavior and write our own C; no code is copied. Keep this credit in About.
- Brush shapes: circle, square, two diagonals, horizontal, vertical; stamp bitmap along a Bresenham line.
- Airbrush: timer (5 ms); each tick sprays 6 + r/5 random single pixels inside radius r.
- Flood fill: scanline with explicit stack; return early when fill color equals target color.
- Rounded rectangle corner radius is capped at 8 px.
- Hidden XP behaviors to include: Ctrl = third color slot; Shift-drag smears a selection,
  Shift-click stamps it; numpad +/- resizes brush; Ctrl+G grid at 4x+; second mouse button cancels
  the current stroke; pasting a larger image expands the canvas.
- XP palette: 28 colors, 14x2 (implemented in `ui/colorbox.c`).

## Milestones
M1 shell + canvas + pencil + palette (done) -> M2 all 16 tools and options ->
M3 file I/O, tile undo/redo, zoom + grid, Attributes dialog, canvas resize handles ->
M4 selection, clipboard, text, Image-menu transforms -> M5 tests, Flatpak, GSettings, i18n,
accessibility (palette is a single drawing area, needs keyboard access), icons, metainfo.

## Known M1 gaps
- Tool icons: `edit-select-lasso-symbolic` and `color-fill-symbolic` are missing from the theme
  (text fallback shown); other stock icons are stand-ins and several tools need custom symbolic SVGs.
- Canvas redraws and re-uploads the whole texture per change. Fine at 800x600; measure before optimizing.
- Pointer events use frame-rate samples joined by lines; sub-frame history not used yet.
