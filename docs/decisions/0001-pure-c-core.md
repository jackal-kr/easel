# 0001 Pure-C core separate from UI
Status: accepted
`src/core` depends on libc only. Drawing logic is testable without a display and cannot leak into widgets.
Cost: a thin conversion layer in ui (e.g. file I/O, colors). Enforced by review and by core tests building without GTK.
