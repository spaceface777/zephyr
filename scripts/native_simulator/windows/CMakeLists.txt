# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)
include("${NSI_CONFIG}")
# --build reads the build tool from the cache, not from normal scope variables.
set(CMAKE_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}" CACHE FILEPATH "Selected Ninja" FORCE)

# Neither environment flags nor a Zephyr toolchain file belong to this project.
set(CMAKE_TOOLCHAIN_FILE "" CACHE FILEPATH "Runner toolchain file" FORCE)
set(CMAKE_C_FLAGS "" CACHE STRING "Runner C flags" FORCE)
foreach(configuration DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_C_FLAGS_${configuration} "" CACHE STRING "Runner C flags" FORCE)
endforeach()
project(native_sim_windows_runner LANGUAGES C)
if(NOT CMAKE_HOST_WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "The Windows runner requires native Windows and MinGW GCC.")
endif()

file(GLOB runner_sources CONFIGURE_DEPENDS
  "${NSI_DIR}/common/src/*.c"
  "${NSI_DIR}/native/src/*.c"
)
foreach(source IN LISTS NSI_INTERFACE_SOURCES)
  if(source MATCHES "\\.h$")
    set_source_files_properties("${source}" PROPERTIES HEADER_FILE_ONLY TRUE)
  elseif(NOT source MATCHES "\\.c$")
    message(FATAL_ERROR "The Windows runner supports only C sources and headers: ${source}")
  endif()
endforeach()
add_library(runner STATIC ${runner_sources} ${NSI_INTERFACE_SOURCES})
set_target_properties(runner PROPERTIES PREFIX "" OUTPUT_NAME runner SUFFIX ".a"
  C_STANDARD 11 C_STANDARD_REQUIRED TRUE C_EXTENSIONS FALSE
)
target_compile_definitions(runner PRIVATE
  NSI_RUNNER_BUILD NSI_N_CPUS=1
  _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=600 _XOPEN_SOURCE_EXTENDED
  ${NSI_INTERFACE_COMPILE_DEFINITIONS}
)
target_include_directories(runner PRIVATE
  "${NSI_DIR}/common/src/include"
  "${NSI_DIR}/common/src"
  "${NSI_DIR}/native/src/include"
  ${NSI_INTERFACE_INCLUDE_DIRECTORIES}
)
target_include_directories(runner SYSTEM PRIVATE ${NSI_INTERFACE_SYSTEM_INCLUDE_DIRECTORIES})
# Interface options follow defaults, so an explicit optimization option wins.
target_compile_options(runner PRIVATE
  -g -Wall -Wpedantic -O0 -fno-pie -fno-pic -ffunction-sections -fdata-sections
  ${NSI_INTERFACE_COMPILE_OPTIONS}
)
add_custom_target(runner_lib DEPENDS runner)

# A first runner-only build can precede the embedded image. Once its sidecar
# exists, retain the final graph across configurations so Ninja keeps depfile state.
# The final build requires the sidecar and must not skip missing inputs.
if(NOT NSI_BUILD_EXE OR
   (NOT NSI_LINK_INPUTS_READY AND NOT EXISTS "${NSI_EMBEDDED}.coff-inputs"))
  return()
endif()

set(localized "${CMAKE_CURRENT_BINARY_DIR}/embedded.loc_cpusw.o")
set(localized_script "${localized}.ld")
set(base_script "${CMAKE_CURRENT_BINARY_DIR}/linker_script.ld")
set(final_script "${CMAKE_CURRENT_BINARY_DIR}/windows_linker_script.ld")
set(inputs "${NSI_EMBEDDED}.coff-inputs")
# Keep the actual original object/archive inputs in the localization dependency graph.
get_filename_component(helper_dir "${NSI_COFF_HELPER}" DIRECTORY)
file(STRINGS "${inputs}" coff_inputs)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${inputs}")
add_custom_command(OUTPUT "${localized}" "${localized_script}"
  COMMAND "${NSI_PYTHON}" "${NSI_COFF_HELPER}"
    --inputs "${inputs}" --input "${NSI_EMBEDDED}" --output "${localized}"
    --linker-script "${localized_script}" --objcopy "${CMAKE_OBJCOPY}"
  DEPENDS "${NSI_EMBEDDED}" "${inputs}" ${coff_inputs}
    "${NSI_COFF_HELPER}"
    "${helper_dir}/../pylib/build_helpers/coff_parser.py"
  VERBATIM
)
add_custom_command(OUTPUT "${base_script}"
  COMMAND "${CMAKE_C_COMPILER}" -x c -E -P
    "${NSI_DIR}/common/other/linker_script.pre.ld" -o "${base_script}"
    -MMD -MP -MF "${base_script}.d" -MT "${base_script}"
  DEPENDS "${NSI_DIR}/common/other/linker_script.pre.ld"
  DEPFILE "${base_script}.d"
  VERBATIM
)
add_custom_command(OUTPUT "${final_script}"
  COMMAND "${CMAKE_COMMAND}" "-DNSI_BASE=${base_script}"
    "-DNSI_LOCALIZED=${localized_script}" "-DNSI_OUTPUT=${final_script}"
    -P "${CMAKE_CURRENT_LIST_DIR}/linker_script.cmake"
  DEPENDS "${base_script}" "${localized_script}"
    "${CMAKE_CURRENT_LIST_DIR}/linker_script.cmake"
  VERBATIM
)

# CMake's SHELL: option syntax uses UNIX_COMMAND parsing on every host.
set(link_options "")
foreach(option IN LISTS NSI_INTERFACE_LINK_OPTIONS)
  if(option MATCHES "^SHELL:(.*)")
    separate_arguments(option UNIX_COMMAND "${CMAKE_MATCH_1}")
  endif()
  list(APPEND link_options ${option})
endforeach()
add_custom_command(OUTPUT "${NSI_EXE}"
  COMMAND "${CMAKE_C_COMPILER}"
    -Wl,--whole-archive "${localized}" "$<TARGET_FILE:runner>" ${NSI_LIBRARIES}
    -Wl,--no-whole-archive -o "${NSI_EXE}"
    -fno-pie -fno-pic -Wall -Wpedantic -Wl,--gc-sections -pthread
    ${link_options} -lm -T "${final_script}"
  DEPENDS runner "${localized}" ${NSI_LIBRARIES} "${final_script}"
  COMMAND_EXPAND_LISTS
  VERBATIM
)
add_custom_target(link_with_esw DEPENDS "${NSI_EXE}")
