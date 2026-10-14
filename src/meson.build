src_inc = include_directories('.')

# Engine: pure C, no GTK. Everything under core/ must stay UI-free.
core_lib = static_library('easel-core',
  ['core/document.c', 'core/raster.c', 'core/history.c', 'core/clip.c'],
  include_directories: src_inc,
)

core_dep = declare_dependency(
  link_with: core_lib,
  include_directories: src_inc,
)

executable('easel',
  [
    'main.c',
    'ui/window.c',
    'ui/canvas.c',
    'ui/cursors.c',
    'ui/colorbox.c',
    'ui/toolbox.c',
    'ui/fileio.c',
    easel_resources,
  ],
  dependencies: [gtk_dep, adw_dep, pixbuf_dep, core_dep, m_dep],
  install: true,
)
