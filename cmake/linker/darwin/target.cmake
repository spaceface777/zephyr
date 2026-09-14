# SPDX-License-Identifier: Apache-2.0

set_property(TARGET linker PROPERTY devices_start_symbol "_device_list_start")
find_program(CMAKE_LINKER ld REQUIRED)
set_ifndef(LINKERFLAGPREFIX -Wl)

macro(configure_linker_script linker_script_gen linker_pass_define)
  add_custom_command(
    OUTPUT ${linker_script_gen}
    DEPENDS ${LINKER_SCRIPT} ${AUTOCONF_H} ${ARGN}
    COMMAND ${CMAKE_COMMAND} -E touch ${linker_script_gen}
    VERBATIM
  )
endmacro()

function(toolchain_ld_force_undefined_symbols)
  foreach(symbol ${ARGN})
    zephyr_link_libraries(${LINKERFLAGPREFIX},-u,_${symbol})
  endforeach()
endfunction()

function(toolchain_ld_link_elf)
  cmake_parse_arguments(
    TOOLCHAIN_LD_LINK_ELF
    ""
    "TARGET_ELF;OUTPUT_MAP;LINKER_SCRIPT"
    "LIBRARIES_PRE_SCRIPT;LIBRARIES_POST_SCRIPT;DEPENDENCIES"
    ${ARGN}
  )

  target_link_options(${TOOLCHAIN_LD_LINK_ELF_TARGET_ELF} PRIVATE
    -nostdlib
    ${LINKERFLAGPREFIX},-r
    ${LINKERFLAGPREFIX},-map,${TOOLCHAIN_LD_LINK_ELF_OUTPUT_MAP}
  )
  add_dependencies(${TOOLCHAIN_LD_LINK_ELF_TARGET_ELF} ${WHOLE_ARCHIVE_LIBS})
  set(macho_whole_archive_libs)
  set(macho_whole_archive_files)
  foreach(lib ${WHOLE_ARCHIVE_LIBS})
    list(APPEND macho_whole_archive_libs
      ${LINKERFLAGPREFIX},-force_load,$<TARGET_FILE:${lib}>
    )
    list(APPEND macho_whole_archive_files $<TARGET_FILE:${lib}>)
  endforeach()

  get_property(zephyr_std_libs TARGET linker PROPERTY lib_include_dir)
  get_property(link_order TARGET linker PROPERTY link_order_library)
  foreach(lib ${link_order})
    get_property(link_flag TARGET linker PROPERTY ${lib}_library)
    list(APPEND zephyr_std_libs ${link_flag})
  endforeach()

  target_link_libraries(
    ${TOOLCHAIN_LD_LINK_ELF_TARGET_ELF}
    ${TOOLCHAIN_LD_LINK_ELF_LIBRARIES_PRE_SCRIPT}
    ${macho_whole_archive_libs}
    ${NO_WHOLE_ARCHIVE_LIBS}
    $<TARGET_OBJECTS:${OFFSETS_LIB}>
    ${TOOLCHAIN_LD_LINK_ELF_LIBRARIES_POST_SCRIPT}
    ${zephyr_std_libs}
  )
  set_property(TARGET ${TOOLCHAIN_LD_LINK_ELF_TARGET_ELF} APPEND PROPERTY
    LINK_DEPENDS
      ${macho_whole_archive_files}
      ${TOOLCHAIN_LD_LINK_ELF_LINKER_SCRIPT}
      ${ZEPHYR_BASE}/scripts/build/macho_link.py
  )
endfunction()

macro(toolchain_linker_finalize)
  set(macho_link_wrapper
    "${PYTHON_EXECUTABLE} ${ZEPHYR_BASE}/scripts/build/macho_link.py --cc <CMAKE_C_COMPILER> --nm ${CMAKE_NM} --"
  )
  set(common_link "<LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
  set(CMAKE_ASM_LINK_EXECUTABLE "${macho_link_wrapper} <FLAGS> <CMAKE_ASM_LINK_FLAGS> ${common_link}")
  set(CMAKE_C_LINK_EXECUTABLE "${macho_link_wrapper} <FLAGS> <CMAKE_C_LINK_FLAGS> ${common_link}")
  set(CMAKE_CXX_LINK_EXECUTABLE "${macho_link_wrapper} <FLAGS> <CMAKE_CXX_LINK_FLAGS> ${common_link}")
endmacro()

function(toolchain_linker_add_compiler_options)
  add_link_options(${ARGV})
endfunction()

include(${ZEPHYR_BASE}/cmake/linker/darwin/target_configure.cmake)
