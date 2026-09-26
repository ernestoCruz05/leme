project('yyjson', 'c', version: '0.12.0',
  default_options: ['c_std=c17', 'warning_level=1', 'werror=false'])
lib = static_library('yyjson', 'src/yyjson.c', install: false)
yyjson_dep = declare_dependency(link_with: lib,
  include_directories: include_directories('src'))
