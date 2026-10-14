# 0004 Paste of an oversize picture

Status: implemented

## Behavior
- Ctrl+V / Paste / Paste From with a picture larger than the canvas in either dimension shows a dialog:
  Resize Canvas to Picture (default), Fit Picture to Canvas, Cancel.
- Fit shrinks the picture smoothly, keeping proportions; the canvas keeps its size.
- Pictures that fit paste without a dialog. If the clipboard has no picture but has picture files, the first is used.
- Pan tool: right-click opens a menu (Paste, Paste From, Select All, Zoom In/Out, Normal Size, Clear Image).

## Acceptance
- [x] Dialog appears for a larger picture; each choice behaves as described (manual).
- [x] Pan right-click menu opens on release and its items work (manual).
- [x] Strings translated (pl, uk); `tools/check-i18n.sh` passes.

## Implementation
`easel_canvas_set_oversize_handler`, `easel_canvas_paste_clip_as` in `canvas-selection.c`; dialog in `window.c`;
`pan_menu` in `data/menus.ui`.
