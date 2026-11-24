project('cd358-911-dispatcher', 'c',
  version : '3.0.0',
  default_options : ['warning_level=2'])

c = meson.get_compiler('c')
mutekix_lib = c.find_library('mutekix', required : true)
elf2bestape = find_program('elf2bestape', required: false)
romtool = find_program('romtool', required: false)

game_elf = executable('cd358-911',
  ['inflate_tiny.c', 'cases_data.c', 'game_911.c', 'platform_muteki.c'],
  name_suffix: 'elf',
  dependencies: mutekix_lib,
  install : false,
  c_args: ['-O3', '-ffunction-sections', '-fdata-sections'],
  link_args: ['-Wl,--gc-sections'])

if romtool.found() and elf2bestape.found()
  romspec = custom_target('romspec',
    input: 'romspec.toml',
    output: 'romspec.rom',
    command: [romtool, 'build', '@INPUT@', '-o', '@OUTPUT@'],
    build_by_default: true)

  custom_target('cd358-911-bestape',
    input: [game_elf, romspec],
    output: 'cd358-911.exe',
    command: [elf2bestape, '-o', '@OUTPUT@', '-r', '@INPUT1@', '@INPUT0@'],
    build_by_default: true)
endif
