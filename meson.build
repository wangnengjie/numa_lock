project('numa_lock', 'cpp',
  version : '0.1',
  default_options : [
    'warning_level=3',
    'cpp_std=c++17',
  ]
)

cc = meson.get_compiler('cpp')
abt_dep = dependency('argobots')
numa_dep = cc.find_library('numa')

incs = include_directories('include')

subdir('src')

exe = executable(
  'numa_lock',
  sources: ['numa_lock.cc', srcs],
  include_directories: incs,
  dependencies: [abt_dep, numa_dep],
  cpp_args: '-faligned-new',
)
