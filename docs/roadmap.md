# Roadmap

Status as of 2026-10-14. Update when scope changes.

## Done
M1 shell/canvas/palette; M2 all 16 tools + options; M3 file I/O, undo/redo, zoom + grid, Attributes,
resize handles; M4 selection, clipboard, text, Image-menu transforms; Preferences (GSettings), header-bar UI,
shortcuts window, thumbnail, frame time, print, context menus, eraser Opaque/Transparent, WebP, undo budget pref.
Refactor stages 1-3: shared constants, enums, shadow undo base, texture buffer pool, file splits, tool table, canvas tests.

## Next (see docs/specs)
1. ~~**0001 Packaging**~~ — done locally: icon, `.desktop`, metainfo, install rules, `.deb`, Flatpak. Left: screenshots for metainfo, Flathub submission, release automation.
2. **0002 i18n** — gettext wiring, extract strings, `po/`.
3. **0003 Accessibility** — accessible names/roles for canvas, palette, toolbox; keyboard access to palette and canvas drawing.
4. CI (needs libadwaita >= 1.8 — container or Flatpak SDK; the Flatpak build could run in CI).

## Ideas / backlog
- GIF save (gdk-pixbuf cannot write GIF; needs own encoder or dropping it).
- Live-apply of undo memory preference.
- Disable text-menu Cut/Copy/Delete when nothing is selected.
- Curve/polygon edge cases vs XP; Edit Colors discoverability (open question).
