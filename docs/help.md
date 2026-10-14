# Easel Help

Easel is a simple paint program for GNOME in the spirit of classic Windows Paint. This page covers the
tools, the menus and the keyboard shortcuts. Press **Ctrl+?** (or **Ctrl+F1**) in the app for the shortcuts window.

## The window
- **Header bar** — New, Open, Save, Undo, Redo on the left; the **Image** menu, the **View** menu and the main
  menu (☰) on the right.
- **Tool box** (left) — the 18 tools. Below the tools, the options of the current tool (sizes, shapes, fill style, …).
- **Canvas** (center) — your picture. Drag the handles on the right, bottom and bottom-right edges to change its size.
- **Color box** (bottom) — the palette and the foreground/background swatches.
- **Status bar** — a hint for the current tool, pointer position, selection size, zoom and picture size.

Panels can be hidden from the View menu or from Preferences.

## Colors
- **Left click** a color: foreground. **Right click**: background. Tools draw with the foreground using the left
  mouse button and with the background using the right button.
- **Double-click** a palette color, press **F2** on it, or click a swatch to change it with the color chooser.
  Changed palette colors are remembered.
- **Keyboard**: Tab to the palette, arrows to move, **Enter/Space** picks the foreground, **Shift+Enter** the background.
- **Ctrl+Shift+X** swaps foreground and background.

## Tools
| Tool | What it does |
|---|---|
| Pan | Drag to move around a large or zoomed picture. |
| Magnifier | Click to toggle between 100 % and the Magnifier zoom chosen in its options (the click point stays in view). |
| Select / Free-Form Select | Select a rectangle or an outline; drag the selection to move it, **Ctrl+drag** to move a copy. Right-click it for a menu. |
| Eraser | Erases to the background color or, in *Transparent* mode, to transparency. **Right button** replaces only the foreground color with the background (color eraser). |
| Fill | Fills an area (left: foreground, right: background). |
| Pick Color | Takes a color from the picture (left: foreground, right: background). |
| Gradient | Blend from foreground to background; drag to set direction and length, Enter to apply, Esc to cancel. |
| Pencil / Brush / Airbrush | Freehand drawing with the chosen size (and brush shape). |
| Text | Drag a box, type. Choose font, size and style; right-click the box for Cut/Copy/Paste. Opaque keeps a background, transparent lets the picture show through. |
| Line / Curve | Line: drag. Curve: drag a line, then drag to bend it (up to two bends). |
| Rectangle / Rounded Rectangle / Ellipse | Drag; **Shift** keeps it square or round. Fill style: outline, filled, or both. |
| Polygon | Drag the first edge, then click to add corners; double-click to close the shape. |

Selection options (below the tool box) choose **Opaque** (the selection's background color is kept) or
**Transparent** (that color is treated as see-through when moved).

## Menus
**Image** — Flip/Rotate, Stretch/Skew, Invert Colors, Attributes (size and background), Clear Image, Draw Opaque.
**View** — zoom (including Normal, Large, Custom), Show Grid, Show Thumbnail, show/hide panels, Frame Time.
**Main menu** — New, Save As, Print, Edit (Cut, Copy, Paste, Clear Selection, Select All), Copy To / Paste From file,
Preferences, Keyboard Shortcuts, About.

Flip, rotate and invert act on the selection when there is one, otherwise on the whole picture.

## Files
Open and save PNG, JPEG, BMP, TIFF and WebP (WebP when your system supports writing it). GIF files can be opened
(first frame only). PNG, TIFF and WebP keep transparency; JPEG and BMP are saved on a white background.
If a picture has unsaved changes, Easel asks before New, Open or closing.

## Zoom and grid
**Ctrl + mouse wheel** zooms (10 %–800 %). **Ctrl+PageUp** large size, **Ctrl+PageDown** normal size.
**Ctrl+G** shows a pixel grid when zoomed in far enough. The thumbnail window shows the whole picture at 100 %.

## Keyboard shortcuts
| Action | Keys |
|---|---|
| New / Open / Save / Save As | Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S |
| Print | Ctrl+P |
| Undo / Redo | Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) |
| Cut / Copy / Paste / Select All | Ctrl+X / Ctrl+C / Ctrl+V / Ctrl+A |
| Clear Selection | Delete |
| Flip/Rotate / Invert Colors / Attributes | Ctrl+R / Ctrl+I / Ctrl+E |
| Clear Image | Ctrl+Shift+N |
| Swap colors | Ctrl+Shift+X |
| Show grid | Ctrl+G |
| Large / Normal size | Ctrl+PageUp / Ctrl+PageDown |
| Zoom | Ctrl+mouse wheel |
| Preferences | Ctrl+, |
| Help | F1 |
| Keyboard shortcuts | Ctrl+? or Ctrl+F1 |
| Quit | Ctrl+Q |

## Preferences (Ctrl+,)
- **Appearance** — light/dark/system, workspace color, which panels are shown, frame time.
- **New Picture** — default size and background.
- **Drawing** — default line width, Magnifier zoom, undo memory, text font and style. Most apply the next time Easel starts.
- **Palette** — reset to the classic 28 colors.

## Undo
Undo and redo have no fixed step limit; the history is limited by memory (Preferences → Drawing → Undo memory, default 512 MB).
