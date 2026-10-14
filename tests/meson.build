test_raster = executable('test-raster', 'test_raster.c', dependencies: [core_dep, m_dep])
test('raster', test_raster)

# Canvas tests link the canvas sources themselves; they need a display to run
# and are skipped without one.
test_canvas = executable('test-canvas',
  ['test_canvas.c',
   '../src/ui/canvas.c', '../src/ui/canvas-selection.c', '../src/ui/canvas-text.c',
   '../src/ui/canvas-shapes.c', '../src/ui/canvas-transform.c', '../src/ui/cursors.c', '../src/ui/tool.c'],
  include_directories: src_inc,
  dependencies: [gtk_dep, core_dep, m_dep],
)
test('canvas', test_canvas, env: {'GSK_RENDERER': 'cairo'}, is_parallel: false)
