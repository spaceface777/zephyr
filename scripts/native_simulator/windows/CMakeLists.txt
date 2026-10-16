# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)
include("${NSI_CONFIG}")
# --build reads the build tool from the cache, not from normal scope variables.
set(CMAKE_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}" CACHE FILEPATH "Selected Ninja" FORCE)
if(NSI_FIBER_SESSION_EXTERNAL AND NSI_BUILD_EXE)
  message(FATAL_ERROR "External Fibers session provider requires library-only mode.")
endif()

# Neither environment flags nor a Zephyr toolchain file belong to this project.
set(CMAKE_TOOLCHAIN_FILE "" CACHE FILEPATH "Runner toolchain file" FORCE)
foreach(language C CXX ASM)
  set(CMAKE_${language}_FLAGS "" CACHE STRING "Runner ${language} flags" FORCE)
  foreach(configuration DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_${language}_FLAGS_${configuration} "" CACHE STRING "Runner ${language} flags" FORCE)
  endforeach()
endforeach()
project(native_sim_windows_runner LANGUAGES C)
if(NOT CMAKE_HOST_WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "The Windows runner requires native Windows and MinGW GCC.")
endif()

file(GLOB runner_sources CONFIGURE_DEPENDS
  "${NSI_DIR}/common/src/*.c"
  "${NSI_DIR}/native/src/*.c"
)
list(APPEND runner_sources "${CMAKE_CURRENT_LIST_DIR}/random.c")
# Mach-O section adapters have no implementation on the PE host.
list(REMOVE_ITEM runner_sources
  "${NSI_DIR}/common/src/nsi_hw_scheduler_macho.c"
)

if(NSI_NCE_BACKEND STREQUAL "coroutines")
  if(NOT NSI_NCT_BACKEND STREQUAL "coroutines")
    message(FATAL_ERROR "Windows coroutine NCE requires coroutine NCT.")
  endif()
  list(REMOVE_ITEM runner_sources "${NSI_DIR}/common/src/nce.c")
  list(APPEND runner_sources "${NSI_DIR}/common/coro/nce_coro.c")
elseif(NOT NSI_NCE_BACKEND STREQUAL "pthreads")
  message(FATAL_ERROR "The Windows runner supports pthread or coroutine NCE only.")
endif()
set(runner_libraries "")
if(NSI_NCT_BACKEND STREQUAL "coroutines")
  list(REMOVE_ITEM runner_sources "${NSI_DIR}/common/src/nct.c")
  list(APPEND runner_sources "${NSI_DIR}/common/coro/nct_coro.c")
  if(NSI_CORO_BACKEND STREQUAL "fibers")
    list(APPEND runner_sources "${NSI_DIR}/common/coro/nsi_fiber_ctx.c")
    if(NOT NSI_FIBER_SESSION_EXTERNAL)
      list(APPEND runner_sources "${NSI_DIR}/common/coro/nsi_fiber_session.c")
    endif()
  elseif(NSI_CORO_BACKEND STREQUAL "asm")
    enable_language(ASM)
    list(APPEND runner_sources
      "${NSI_DIR}/common/coro/nsi_coro_ctx.c"
      "${NSI_DIR}/common/coro/nsi_coro.S"
    )
  elseif(NSI_CORO_BACKEND STREQUAL "boost")
    enable_language(CXX)
    list(APPEND runner_sources
      "${NSI_DIR}/common/coro/nsi_coro_ctx.c"
      "${NSI_DIR}/common/coro/nsi_coro_boost.cpp"
    )
    if(NOT NSI_BOOST_CONTEXT_LIB)
      message(FATAL_ERROR "The Boost.Context primitive requires the Boost.Context library.")
    endif()
    set(runner_libraries "${NSI_BOOST_CONTEXT_LIB}")
  else()
    message(FATAL_ERROR "Windows coroutine NCT supports Fibers, the assembly switch "
                        "and Boost.Context only.")
  endif()
elseif(NOT NSI_NCT_BACKEND STREQUAL "pthreads")
  message(FATAL_ERROR "The Windows runner supports pthread or coroutine NCT only.")
endif()

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
  $<$<STREQUAL:${NSI_NCE_BACKEND},pthreads>:NSI_NCE_BACKEND_PTHREADS>
  $<$<STREQUAL:${NSI_NCE_BACKEND},coroutines>:NSI_NCE_BACKEND_COROUTINES>
  $<$<STREQUAL:${NSI_NCT_BACKEND},pthreads>:NSI_NCT_BACKEND_PTHREADS>
  $<$<STREQUAL:${NSI_CORO_BACKEND},fibers>:NSI_CORO_FIBERS>
  _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=600 _XOPEN_SOURCE_EXTENDED
  ${NSI_INTERFACE_COMPILE_DEFINITIONS}
)
target_include_directories(runner PRIVATE
  "${NSI_DIR}/common/src/include"
  "${NSI_DIR}/common/coro"
  "${NSI_DIR}/common/src"
  "${NSI_DIR}/native/src/include"
  ${NSI_INTERFACE_INCLUDE_DIRECTORIES}
)
target_include_directories(runner SYSTEM PRIVATE
  ${NSI_INTERFACE_SYSTEM_INCLUDE_DIRECTORIES}
  "$<$<COMPILE_LANGUAGE:CXX>:${NSI_BOOST_INCLUDE_DIRS}>"
)
# Interface options follow defaults, so an explicit optimization option wins.
target_compile_options(runner PRIVATE
  -g -Wall -Wpedantic -O0 -fno-pie -fno-pic -ffunction-sections -fdata-sections
  $<$<STREQUAL:${NSI_CORO_BACKEND},fibers>:-msse2>
  # The C++ primitive uses no C++ runtime, so the C link needs no libstdc++.
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
  ${NSI_INTERFACE_COMPILE_OPTIONS}
)
add_custom_target(runner_lib DEPENDS runner)

# A first runner-only build can precede the embedded image. Once its sidecar
# exists, retain the final graph across configurations so Ninja keeps depfile state.
# The final build requires the sidecar and must not skip missing inputs.
if(NOT NSI_LINK_INPUTS_READY AND NOT EXISTS "${NSI_EMBEDDED}.coff-inputs")
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

# Custom image links need these inputs even when the runner does not link an executable.
add_custom_target(link_inputs DEPENDS
  "${localized}"
  "${final_script}"
)
if(NOT NSI_BUILD_EXE)
  return()
endif()

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
    -Wl,--no-whole-archive ${runner_libraries} -o "${NSI_EXE}"
    -fno-pie -fno-pic -Wall -Wpedantic -Wl,--gc-sections -pthread
    ${link_options} -lm -T "${final_script}"
  DEPENDS runner "${localized}" ${NSI_LIBRARIES} ${runner_libraries} "${final_script}"
  COMMAND_EXPAND_LISTS
  VERBATIM
)
add_custom_target(link_with_esw DEPENDS "${NSI_EXE}")
