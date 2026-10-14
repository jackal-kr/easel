# 0001 Packaging

Status: draft
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
- [ ] `meson setup --prefix=/tmp/p build-p && ninja -C build-p install` yields a runnable tree; app starts with prefs working.
- [ ] `desktop-file-validate` and `appstreamcli validate` pass.
- [ ] `flatpak-builder` produces a bundle that opens, saves and prints a picture.
- [ ] Opening a PNG from Files (double click) launches Easel with the file.

## Test plan
Automated: the two validators as meson tests when available. Manual: install the Flatpak, run the checklist.

## Out of scope
Publishing to Flathub, translations (spec 0002), accessibility (spec 0003).
