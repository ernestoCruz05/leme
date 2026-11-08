project(
  'leme', 'c',
  version: '0.3.0',
  default_options: ['c_std=c17', 'warning_level=3', 'werror=true'],
)

add_project_arguments('-D_POSIX_C_SOURCE=200809L', '-DWLR_USE_UNSTABLE',
  '-DLEME_VERSION="@0@"'.format(meson.project_version()), language: 'c')

leme_version_header = vcs_tag(
  input: 'src/version.h.in',
  output: 'leme-version.h',
  command: ['git', 'describe', '--always', '--dirty', '--exclude=*'],
  fallback: 'unknown',
)

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
  wlroots_proj = subproject('wlroots',
    default_options: ['examples=false', 'default_library=static'])
  wlroots_dep = wlroots_proj.get_variable('wlroots')
  add_project_arguments('-DLEME_HAVE_EFFECTS', language: 'c')
else
  wlroots_dep = dependency('wlroots-0.20', version: ['>=0.20.0', '<0.21.0'],
    allow_fallback: false)
endif
wayland_server_dep = dependency('wayland-server', version: '>=1.22')
xkbcommon_dep = dependency('xkbcommon', version: '>=1.5')
libinput_dep = dependency('libinput')
pixman_dep = dependency('pixman-1')
libdrm_dep = dependency('libdrm')
libudev_dep = dependency('libudev')
wayland_scanner = find_program('wayland-scanner')
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
decoration_xml = files('protocols/leme-decoration-v1.xml')
decoration_header = custom_target(
  'leme-decoration-server-header',
  input: decoration_xml,
  output: 'leme-decoration-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
decoration_code = custom_target(
  'leme-decoration-code',
  input: decoration_xml,
  output: 'leme-decoration-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

leme_public_math_dep = cc.find_library('m', required: true)
leme_public_core_sources = files(
  'src/public/value.c',
  'src/public/arena.c',
  'src/public/utf8.c',
)

leme_sources = files(
  'src/core/server.c',
  'src/core/command.c',
  'src/core/command_parse.c',
  'src/core/gate.c',
  'src/core/process.c',
  'src/core/session_environment.c',
  'src/config/check.c',
  'src/config/config.c',
  'src/config/diagnostics.c',
  'src/config/envfile.c',
  'src/config/expand.c',
  'src/config/parse.c',
  'src/config/style.c',
  'src/config/style-parse.c',
  'src/config/desktop-menu.c',
  'src/config/render.c',
  'src/config/source_table.c',
  'src/config/scfg_lex.c',
  'src/config/scfg_parse.c',
  'src/config/scfg_source.c',
  'src/config/validate.c',
  'src/config/candidate.c',
  'src/config/live.c',
  'src/config/prepare.c',
  'src/input/gesture_accel.c',
  'src/input/input.c',
  'src/input/keyboard.c',
  'src/input/public.c',
  'src/config/public-scalars.c',
  'src/config/public-rules.c',
  'src/config/public-bindings.c',
  'src/config/public-animation.c',
  'src/config/public-diagnostics.c',
  'src/config/public.c',
  'src/input/edge.c',
  'src/input/edge_tracker.c',
  'src/input/gesture.c',
  'src/input/pointer.c',
  'src/input/swipe_tracker.c',
  'src/input/control.c',
  'src/output/gpu.c',
  'src/output/output.c',
  'src/output/public.c',
  'src/output/placement.c',
  'src/output/home.c',
  'src/output/control.c',
  'src/render/graphics.c',
  'src/workspace/tag.c',
  'src/workspace/public.c',
  'src/workspace/control.c',
  'src/workspace/layout.c',
  'src/shell/policy.c',
  'src/shell/ownership.c',
  'src/shell/scratchpad.c',
  'src/shell/sticky.c',
  'src/shell/view.c',
  'src/shell/minimize.c',
  'src/shell/maximize.c',
  'src/shell/placement.c',
  'src/shell/container-policy.c',
  'src/shell/container.c',
  'src/render/container.c',
  'src/desktop/frame.c',
  'src/shell/public.c',
  'src/shell/control.c',
  'src/shell/xdg.c',
  'src/shell/layer.c',
  'src/shell/layer_layout.c',
  'src/shell/xwayland.c',
  'src/shell/rules.c',
  'src/protocols/data.c',
  'src/protocols/decoration.c',
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
  'src/render/timing.c',
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
leme_sources += [output_power_header, decoration_header, decoration_code]
leme_sources += leme_public_core_sources
leme_public_model_sources = files('src/public/schema.c', 'src/public/model.c', 'src/public/model-cache.c', 'src/public/identity.c')
leme_control_core_sources = files('src/control/limits.c', 'src/control/decode.c', 'src/control/number.c', 'src/control/source.c', 'src/control/request.c', 'src/control/registry.c', 'src/control/types.c', 'src/control/control.c', 'src/control/expr.c', 'src/control/validate.c', 'src/control/eval.c', 'src/control/eval-scalar.c', 'src/control/eval-lookup.c', 'src/control/eval-collection.c', 'src/control/act.c', 'src/core/command_parse.c')
leme_control_ipc_sources = files('src/control/reply.c', 'src/ipc/frame.c', 'src/ipc/queue.c', 'src/ipc/connection.c', 'src/ipc/schedule.c', 'src/control/dispatch.c', 'src/control/watch.c', 'src/control/watch-event.c', 'src/ipc/watch-peer.c')
leme_sources += files('src/public/json.c', 'src/public/server.c') + leme_public_model_sources + leme_control_core_sources + leme_control_ipc_sources

yyjson_dep = dependency('yyjson', version: '>=0.12.0',
  fallback: ['yyjson', 'yyjson_dep'])

leme_desk_gio_dep = dependency('gio-2.0')
leme_desk_cairo_dep = dependency('cairo')
leme_desk_pango_dep = dependency('pangocairo')
leme_desk_rsvg_dep = dependency('librsvg-2.0', version: '>=2.46')
leme_desk_catalog_sources = files('src/desktop/catalog.c', 'src/desktop/positions.c', 'src/desktop/theme.c', 'src/desktop/app-icon.c')
leme_desk_pixel_sources = files('src/desktop/pixels.c', 'src/desktop/icons.c')
leme_desk_watch_sources = files('src/desktop/watch.c')
leme_desk_file_sources = files('src/desktop/fileops.c')
leme_desk_scene_sources = files('src/desktop/desktop.c', 'src/desktop/grid.c', 'src/desktop/open.c', 'src/desktop/strip.c', 'src/desktop/decoration.c', 'src/desktop/buttons.c', 'src/desktop/owner.c', 'src/desktop/target.c', 'src/desktop/drag.c', 'src/desktop/menu.c', 'src/desktop/prompt.c') + leme_desk_watch_sources
leme_sources += leme_desk_catalog_sources + leme_desk_pixel_sources + leme_desk_scene_sources + leme_desk_file_sources

leme_dependencies = [
  leme_desk_gio_dep,
  leme_desk_cairo_dep,
  leme_desk_pango_dep,
  leme_desk_rsvg_dep,
  wlroots_dep,
  wayland_server_dep,
  xkbcommon_dep,
  libinput_dep,
  pixman_dep,
  libdrm_dep,
  libudev_dep,
  leme_public_math_dep,
  yyjson_dep,
]

leme_exe = executable(
  'leme',
  'src/main.c',
  leme_sources,
  layer_shell_header,
  leme_version_header,
  include_directories: leme_inc,
  dependencies: leme_dependencies,
  install: true,
)

timao_exe = executable(
  'timao',
  'src/timao/main.c',
  'src/timao/request.c',
  'src/timao/output.c',
  'src/timao/endpoint.c',
  include_directories: leme_inc,
  c_args: ['-D_GNU_SOURCE'],
  dependencies: [yyjson_dep],
  install: true,
)

install_data(
  'session/leme-session',
  install_dir: get_option('bindir'),
  install_mode: 'rwxr-xr-x',
  install_tag: 'runtime',
)
install_data(
  'session/leme.desktop',
  install_dir: get_option('datadir') / 'wayland-sessions',
  install_tag: 'runtime',
)
install_data(
  'session/leme-portals.conf',
  install_dir: get_option('datadir') / 'xdg-desktop-portal',
  install_tag: 'runtime',
)
systemd_dep = dependency('systemd', required: false)
if systemd_dep.found()
  systemd_user_unit_dir = systemd_dep.get_variable(
    pkgconfig: 'systemduserunitdir',
    pkgconfig_define: ['prefix', get_option('prefix')],
  )
else
  systemd_user_unit_dir = get_option('prefix') / 'lib' / 'systemd' / 'user'
endif
install_data(
  'session/leme-session.target',
  install_dir: systemd_user_unit_dir,
  install_tag: 'runtime',
)

fs = import('fs')
if fs.is_dir('.dev/tests')
  subdir('.dev/tests')
endif
