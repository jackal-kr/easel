# 0001 Packaging

Status: implemented (local builds; Flathub submission out of scope)
Owner: JOD

## Problem
Easel only runs from the build tree. It needs an app icon, desktop integration and a Flatpak so it can be installed.

## Behavior
- App icon (scalable SVG + symbolic) shown in the shell, About dialog and window.
- `io.github.jackal_kr.Easel.desktop`: name, comment, categories Graphics;2DGraphics;RasterGraphics, MIME types for png/jpeg/bmp/tiff/webp/gif.
- AppStream `metainfo.xml` with description, screenshots placeholder, release entry, OARS rating, license.
- `meson install` installs binary, schema (compiled), icons, desktop, metainfo.
- Flatpak manifest on GNOME runtime (>= the one providing libadwaita 1.8), finish-args: wayland, fallback-x11, dri; file access via portal only.

## Acceptance criteria
- [x] `meson setup --prefix=/tmp/p build-p && ninja -C build-p install` yields a runnable tree; app starts with prefs working.
- [x] `desktop-file-validate` and `appstreamcli validate` pass.
- [x] `flatpak-builder` produces a bundle that opens, saves and prints a picture.
- [ ] (manual) Opening a PNG from Files (double click) launches Easel with the file.

## Test plan
Automated: the two validators as meson tests when available. Manual: install the Flatpak, run the checklist.

## Implementation
`data/*.desktop`, `data/*.metainfo.xml`, `data/icons/hicolor`, install rules + validators in `data/meson.build`;
`debian/` (native package, debhelper + meson); `build-aux/flatpak/*.yml` (GNOME 50 runtime); scripts in `tools/`;
VS Code tasks in `.vscode/tasks.json`.

## Out of scope
Publishing to Flathub.
