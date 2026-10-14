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
install_data('../docs/help.html', install_dir: get_option('datadir') / 'doc' / 'easel')
install_data('io.github.jackal_kr.Easel.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')

# Desktop integration: icons, launcher and AppStream metadata.
install_data('icons/hicolor/scalable/apps/@0@.svg'.format(app_id),
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')
install_data('icons/hicolor/symbolic/apps/@0@-symbolic.svg'.format(app_id),
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'symbolic' / 'apps')
desktop_file = i18n.merge_file(
  input: '@0@.desktop.in'.format(app_id),
  output: '@0@.desktop'.format(app_id),
  type: 'desktop',
  po_dir: meson.project_source_root() / 'po',
  install: true,
  install_dir: get_option('datadir') / 'applications',
)
metainfo_file = i18n.merge_file(
  input: '@0@.metainfo.xml.in'.format(app_id),
  output: '@0@.metainfo.xml'.format(app_id),
  type: 'xml',
  po_dir: meson.project_source_root() / 'po',
  install: true,
  install_dir: get_option('datadir') / 'metainfo',
)

desktop_validate = find_program('desktop-file-validate', required: false)
if desktop_validate.found()
  test('validate-desktop', desktop_validate, args: [desktop_file])
endif
appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test('validate-metainfo', appstreamcli, args: ['validate', '--no-net', metainfo_file])
endif

# Refresh the system caches after `meson install` (skipped for staged installs).
gnome.post_install(
  glib_compile_schemas: true,
  gtk_update_icon_cache: true,
  update_desktop_database: true,
)
