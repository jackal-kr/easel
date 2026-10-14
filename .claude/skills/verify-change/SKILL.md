---
name: verify-change
description: Use before reporting any Easel change as done. Builds warning-free, runs all tests, smoke-starts the app, and checks the change against its spec.
---

# Verify a change

Run, in order, and report only failures plus a one-line pass summary:

1. `ninja -C build 2>&1 | grep -E "warning|error" -A3` — must print nothing. (A reconfigure may be needed after editing meson files: `meson setup --reconfigure build`.)
2. `meson test -C build --print-errorlogs` — core tests must pass. The canvas test is skipped without a display; with one, also run
   `GSK_RENDERER=cairo build/tests/test-canvas` ("canvas: all checks passed").
3. Smoke start: `timeout 5 build/src/easel` — exit code 124 (still running) and no GLib-CRITICAL/WARNING lines.
4. If a spec exists in `docs/specs/`, walk its acceptance criteria; say which are covered by tests and which need
   the user to try (UI look/feel cannot be verified headless — say so plainly).
5. New behavior without a test? Add one (core: `tests/test_raster.c`; canvas input: `tests/test_canvas.c`).
6. Do not commit or push unless asked.
