# 0003 Accessibility

Status: implemented (manual Orca check pending)
Owner: JOD

## Problem
Screen readers and keyboard-only users get unnamed controls: the toolbox buttons, the canvas, the palette and the
color swatches have no accessible names, and changes (colors picked, tool changed) are not announced.

## Behavior
- Every control has an accessible name (and description where the tooltip carries it): tool buttons (tool name,
  with the hint as description), option buttons, header-bar buttons, status-bar fields, zoom buttons.
- The canvas exposes a name ("Picture") and a description of its size, zoom and current tool; they update on change.
- The palette exposes its current cell ("Color 5 of 28, Dark Yellow ..." — name from RGB) and updates when keyboard
  focus moves; picking a color announces the new foreground/background.
- The foreground/background swatch widget has a name and a description of the two colors.
- Changing the tool announces its name (live region on the status bar hint).
- Keyboard: every dialog and menu is reachable; palette navigable by arrow keys (done). Drawing with the keyboard is
  **not** part of this spec.

## Acceptance criteria
- [x] Icon buttons (header, zoom, swap, main menu) have accessible labels. (No automated widget-tree test yet.)
- [x] Canvas, palette and swatches have accessible names and live descriptions.
- [x] Palette cell and selection changes update the accessible description.
- [ ] (manual, Orca) tool, color and menu navigation is announced sensibly.

## Design notes
`gtk_accessible_update_property` / `_state` with `GTK_ACCESSIBLE_PROPERTY_LABEL|DESCRIPTION`; status bar hint label uses
`GTK_ACCESSIBLE_ROLE_STATUS`. Color names come from a small table in `ui/config.h` (nearest of the 28 classic names).

## Test plan
Automated: `tests/test_canvas.c` (canvas name/description update) and a widget-tree walk in a new test over the toolbox.
Manual: Orca walkthrough.

## Out of scope
Keyboard-only drawing (virtual cursor), high-contrast theme tuning, magnifier-style zoom follow-focus.
