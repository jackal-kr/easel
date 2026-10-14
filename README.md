# Easel

A native GNOME paint program with the functionality and workflow of classic Windows Paint, built from GNOME
components only: **GTK 4 + libadwaita**, written in C, built with Meson. It follows the GNOME look (header bar,
dark mode, dialogs) rather than imitating the Windows 95/XP appearance.

Status: usable, pre-release (0.1). Debian package and Flatpak can be built locally — see the [roadmap](docs/roadmap.md).

## Features
- All the classic tools: Pencil, Brush (12 shapes), Airbrush, Eraser (opaque or transparent, color eraser), Fill,
  Pick Color, Magnifier, Text, Line, Curve, Rectangle, Rounded Rectangle, Ellipse, Polygon, Select, Free-Form Select —
  plus Pan and Gradient.
- Selections with move/copy, clipboard, flip, rotate, stretch/skew, invert; right-click context menus.
- Unlimited-step undo/redo bounded by memory, zoom 10–800 %, pixel grid, thumbnail, canvas resize handles.
- PNG, JPEG, BMP, TIFF and WebP; GIF can be opened. Printing.
- 28-color palette with editing, Preferences (theme, panels, defaults), keyboard shortcuts window.
- Hard-edged drawing like the original: pixel-exact results.

## Build and run
Requirements: GTK ≥ 4.14, libadwaita ≥ 1.8, gdk-pixbuf 2, GCC or Clang,
Meson, Ninja. Developed on Ubuntu 26.04.

    meson setup build
    ninja -C build
    ./build/src/easel

## Install
Packages are built into `dist/`:

    tools/check-env.sh              # is everything needed installed?
    tools/build-deb.sh              # dist/easel_<version>_amd64.deb   -> sudo apt install ./dist/easel_*.deb
    tools/build-flatpak.sh          # dist/io.github.jackal_kr.Easel.flatpak -> flatpak --user install dist/*.flatpak

In VS Code these are tasks: **Terminal → Run Task…** (check environment, build, test, debug, push, packages).

## Test
    meson test -C build --print-errorlogs

Core tests run anywhere. The canvas tests need a display and are skipped without one; headless-friendly:
`GSK_RENDERER=cairo meson test -C build`.

## Documentation
- [User help](docs/help.md) — tools, menus, shortcuts
- [Architecture](docs/architecture.md), [decisions](docs/decisions/), [specs](docs/specs/), [roadmap](docs/roadmap.md)
- [CLAUDE.md](CLAUDE.md) — working rules for contributors using Claude Code

## Project layout
    src/core/   pure C engine: pixels, rasterizer, history, transforms (no GTK)
    src/ui/     GTK/libadwaita: canvas, toolbox, color box, window, dialogs
    data/       menus, GSettings schema, icons, styles
    tests/      core and canvas tests
    tools/      icon generator

## Credits and license
Easel is inspired by the old, beautiful Microsoft Paint of Windows XP. Behavior of the classic tools was also studied from [jspaint](https://github.com/1j01/jspaint) (MIT); Easel contains
none of its code. Easel is licensed under the GPL-3.0-or-later — see [LICENSE](LICENSE).
