project('easel', 'c',
  version: '1.3.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 1.3',
  default_options: ['c_std=gnu11', 'warning_level=2'],
)

gnome = import('gnome')
i18n = import('i18n')
cc = meson.get_compiler('c')

app_id = 'io.github.jackal_kr.Easel'

gtk_dep = dependency('gtk4', version: '>= 4.14')
adw_dep = dependency('libadwaita-1', version: '>= 1.8')
pixbuf_dep = dependency('gdk-pixbuf-2.0')
m_dep = cc.find_library('m', required: false)

add_project_arguments(
  cc.get_supported_arguments([
    '-Wshadow',
    '-Wstrict-prototypes',
    '-Wmissing-prototypes',
    '-Wno-unused-parameter',  # GTK callbacks have fixed signatures
    '-Wno-missing-field-initializers',  # GActionEntry tables rely on zero fill
  ]),
  '-DEASEL_VERSION="@0@"'.format(meson.project_version()),
  '-DEASEL_DOCDIR="@0@"'.format(get_option('prefix') / get_option('datadir') / 'doc' / 'easel'),
  '-DGETTEXT_PACKAGE="easel"',
  '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
  '-DEASEL_APP_ID="@0@"'.format(app_id),
  language: 'c',
)

subdir('po')
subdir('data')
subdir('src')
subdir('tests')
