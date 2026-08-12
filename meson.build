project(
  'leme', 'c',
  version: '0.1.0',
  default_options: ['c_std=c17', 'warning_level=3', 'werror=true'],
)

add_project_arguments('-D_POSIX_C_SOURCE=200809L', '-DWLR_USE_UNSTABLE', language: 'c')

cc = meson.get_compiler('c')
add_project_arguments(
  cc.get_supported_arguments(
    '-Wconversion',
    '-Wsign-conversion',
    '-Wshadow',
    '-Wvla',
    '-Wcast-qual',
    '-Wpointer-arith',
    '-Wmissing-prototypes',
    '-Wdouble-promotion',
    '-Wwrite-strings',
    '-Wstrict-prototypes',
  ),
  language: 'c',
)

leme_inc = include_directories('src')

if get_option('effects')
  # subproject() rather than a fallback: the patched copy must be certain,
  # not merely preferred over a system library that happens to be present.
  # Static: the patched library must not be findable at runtime by soname.
  # A shared build installs a leme that resolves libwlroots-0.20.so to the
  # unpatched system copy and dies on the first patched symbol.
  wlroots_proj = subproject('wlroots',
    default_options: ['examples=false', 'default_library=static'])
  wlroots_dep = wlroots_proj.get_variable('wlroots')
  add_project_arguments('-DLEME_HAVE_EFFECTS', language: 'c')
else
  wlroots_dep = dependency('wlroots-0.20', version: ['>=0.20.0', '<0.21.0'])
endif
wayland_server_dep = dependency('wayland-server', version: '>=1.22')
xkbcommon_dep = dependency('xkbcommon', version: '>=1.5')
libinput_dep = dependency('libinput')
pixman_dep = dependency('pixman-1')
wayland_scanner = find_program('wayland-scanner')
wayland_protocols_dep = dependency('wayland-protocols')
wayland_protocols_dir = wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir')
layer_shell_header = custom_target(
  'wlr-layer-shell-server-header',
  input: 'protocols/wlr-layer-shell-unstable-v1.xml',
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
output_power_header = custom_target(
  'wlr-output-power-management-server-header',
  input: 'protocols/wlr-output-power-management-unstable-v1.xml',
  output: 'wlr-output-power-management-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_header = custom_target(
  'xdg-shell-client-header',
  input: 'protocols/xdg-shell.xml',
  output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
xdg_shell_code = custom_target(
  'xdg-shell-private-code',
  input: 'protocols/xdg-shell.xml',
  output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

wayland_protocols_staging = join_paths(wayland_protocols_dir, 'staging')
capture_protocols = [
  join_paths(wayland_protocols_staging, 'ext-foreign-toplevel-list',
    'ext-foreign-toplevel-list-v1.xml'),
  join_paths(wayland_protocols_staging, 'ext-image-capture-source',
    'ext-image-capture-source-v1.xml'),
  join_paths(wayland_protocols_staging, 'ext-image-copy-capture',
    'ext-image-copy-capture-v1.xml'),
]
capture_client_headers = []
capture_client_code = []
foreach protocol : capture_protocols
  capture_client_headers += custom_target(
    '@0@-client-header'.format(protocol.split('/')[-1]),
    input: protocol,
    output: '@0@-client-protocol.h'.format(protocol.split('/')[-1]),
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  capture_client_code += custom_target(
    '@0@-private-code'.format(protocol.split('/')[-1]),
    input: protocol,
    output: '@0@-protocol.c'.format(protocol.split('/')[-1]),
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach
wayland_client_dep = dependency('wayland-client', version: '>=1.22')

leme_sources = files(
  'src/core/server.c',
  'src/core/command.c',
  'src/core/gate.c',
  'src/core/process.c',
  'src/core/session_environment.c',
  'src/config/check.c',
  'src/config/config.c',
  'src/config/diagnostics.c',
  'src/config/parse.c',
  'src/config/scfg_diag.c',
  'src/config/scfg_lex.c',
  'src/config/scfg_parse.c',
  'src/config/scfg_source.c',
  'src/config/validate.c',
  'src/input/input.c',
  'src/input/keyboard.c',
  'src/input/pointer.c',
  'src/output/output.c',
  'src/output/placement.c',
  'src/render/graphics.c',
  'src/workspace/tag.c',
  'src/workspace/layout.c',
  'src/shell/policy.c',
  'src/shell/scratchpad.c',
  'src/shell/view.c',
  'src/shell/xdg.c',
  'src/shell/layer.c',
  'src/shell/layer_layout.c',
  'src/shell/xwayland.c',
  'src/shell/rules.c',
  'src/protocols/data.c',
  'src/protocols/desktop.c',
  'src/protocols/input.c',
  'src/protocols/capture.c',
  'src/protocols/output_power.c',
  'src/protocols/session.c',
  'src/protocols/tearing.c',
  'src/protocols/publication.c',
  'src/protocols/workspace.c',
  'src/protocols/toplevel.c',
  'src/render/render.c',
  'src/render/animation.c',
  'src/render/workspace_effect.c',
  'src/render/workspace_transition.c',
  'src/render/view.c',
  'src/render/layer.c',
  'src/render/session.c',
  'src/render/xwayland.c',
  'src/ipc/json.c',
  'src/ipc/ipc.c',
  'src/ipc/request.c',
  'src/ipc/state.c',
)
leme_sources += [output_power_header]

leme_dependencies = [
  wlroots_dep,
  wayland_server_dep,
  xkbcommon_dep,
  libinput_dep,
  pixman_dep,
]

leme_exe = executable(
  'leme',
  'src/main.c',
  leme_sources,
  layer_shell_header,
  include_directories: leme_inc,
  dependencies: leme_dependencies,
  install: true,
)

executable(
  'timao',
  'src/timao/main.c',
  include_directories: leme_inc,
  install: true,
)

install_data(
  'session/leme-session',
  install_dir: get_option('bindir'),
  install_mode: 'rwxr-xr-x',
)
install_data(
  'session/leme.desktop',
  install_dir: get_option('datadir') / 'wayland-sessions',
)
install_data(
  'session/leme-portals.conf',
  install_dir: get_option('datadir') / 'xdg-desktop-portal',
)

fs = import('fs')
if fs.is_dir('.dev/tests')
  subdir('.dev/tests')
endif
