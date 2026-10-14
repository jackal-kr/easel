easel_resources = gnome.compile_resources(
  'easel-resources',
  'io.github.jackal_kr.Easel.gresource.xml',
  source_dir: meson.current_source_dir(),
  c_name: 'easel',
)

# Settings schema. Compiled next to the build so the app also runs from the
# build directory (see ui/settings.c), and installed for real use.
custom_target('gschemas',
  input: 'io.github.jackal_kr.Easel.gschema.xml',
  output: 'gschemas.compiled',
  command: [find_program('glib-compile-schemas'), '--strict',
            '--targetdir', meson.current_build_dir(), meson.current_source_dir()],
  build_by_default: true,
)
install_data('io.github.jackal_kr.Easel.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')
