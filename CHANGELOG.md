# Changelog

All notable user-facing changes to Easel. Newest first. Built from the git history; versions follow the tags.

## Unreleased
- New Font dialog with a font list, a live preview, size, text color and Bold/Italic/Underline/Strikethrough in one place.
- Font, size, color and styles can differ within one text box: select part of the text and use Font… or right-click → Font for Selection….
- New languages: Czech, German, Italian, Spanish, Portuguese and Russian.

## 1.1.5
- The Font dialog shows the current font almost at once in the Flatpak (selected as soon as the list loads; other installs pass it in directly).

## 1.1.4
- The Font dialog now shows its font list and highlights the current font (it could come up empty for some fonts, such as Arial, in the Flatpak).

## 1.1.3
- Gradient fades from the chosen color to transparent, so a second gradient lays over the first instead of replacing it.

## 1.1.2
- Help opens as a readable web page in your default browser.
- Faster picture resize and paste, and quicker flip, rotate, stretch and skew on large pictures.
- Committing text no longer copies the whole picture.
- Safer reference counting for the display buffers.

## 1.1.1
- Right-click outside a selection with Select or Free-Form Select opens the Paste menu.

## 1.1.0
- Fit to Window zoom replaces Large Size.
- Grid dialog: cell size and an axonometric grid.
- Stretch and Skew by percent or exact pixels, with Keep aspect ratio.
- Polish and Ukrainian translations updated.

## 1.0.1
- Screenshots added to the AppStream metadata.

## 1.0.0
- Pasting a picture larger than the canvas asks whether to fit it or enlarge the canvas.
- Right-click menu in Pan mode.
- Polish and Ukrainian translations, accessibility labels and announcements.
- Debian and Flatpak packages, app icon, desktop and AppStream metadata.

## 0.1.0 (first packaged version)
- Tools: pencil, brush, airbrush, eraser (with transparent mode), fill, colour picker, line, rectangle, rounded rectangle, ellipse, polygon, curve, gradient, magnifier, pan, text.
- Select and Free-Form Select with move, copy, stretch handles, clipboard, Copy To and Paste From, Draw Opaque and Transparent.
- Image menu: flip, rotate, stretch, skew, invert colours; Edit Colors for the palette.
- Undo and redo with a configurable memory limit.
- Open and save PNG, JPEG, BMP, TIFF and WebP; unsaved-changes prompts; open from the command line.
- Attributes dialog, canvas resize handles, zoom, pixel grid.
- GNOME header bar, Preferences, keyboard shortcuts window, Help.
