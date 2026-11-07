# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

# Native Windows only, with an i686 MinGW compiler and the host C runtime: GCC (the host
# toolchain) or Clang (the host/llvm toolchain), both with GNU ld.
if(NOT NATIVE_SIM_EXPERIMENTAL_WINDOWS)
  message(FATAL_ERROR "Windows native_sim requires NATIVE_SIM_EXPERIMENTAL_WINDOWS=ON.")
endif()
if(NOT CONFIG_BOARD_NATIVE_SIM OR CONFIG_64BIT)
  message(FATAL_ERROR "Experimental Windows support requires native_sim/native (32 bit).")
endif()
if(NOT ZEPHYR_TOOLCHAIN_VARIANT STREQUAL "host" OR
   NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang)$" OR NOT LINKER STREQUAL "ld")
  message(FATAL_ERROR "Windows native_sim requires the host toolchain with i686 MinGW GCC, "
                      "or the host/llvm toolchain with Clang, and GNU ld.")
endif()

# Clang links with GNU ld only through -fuse-ld=bfd, which needs ld.bfd.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND NOT GNULD_LINKER_IS_BFD)
  message(FATAL_ERROR "Windows native_sim with Clang requires GNU ld as "
                      "i686-w64-mingw32-ld.bfd, next to Clang or on PATH. Found: ${GNULD_LINKER}")
endif()

set(windows_target_flags)
if(CMAKE_C_COMPILER_TARGET)
  set(windows_target_flags "--target=${CMAKE_C_COMPILER_TARGET}")
endif()
execute_process(
  COMMAND "${CMAKE_C_COMPILER}" ${windows_target_flags} -dumpmachine
  OUTPUT_VARIABLE windows_triplet
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
  COMMAND "${CMAKE_C_COMPILER}" ${windows_target_flags} -dM -E -x c
    "${ZEPHYR_BASE}/misc/empty_file.c"
  OUTPUT_VARIABLE windows_macros
  COMMAND_ERROR_IS_FATAL ANY
)
# Clang prints the normalized spelling of the same target.
if(NOT windows_triplet MATCHES "^i686-w64-(mingw32|windows-gnu)$" OR
   NOT windows_macros MATCHES "#define _WIN32 1" OR
   windows_macros MATCHES "#define _WIN64" OR
   NOT windows_macros MATCHES "#define __SIZEOF_POINTER__ 4" OR
   NOT windows_macros MATCHES "#define __SIZEOF_LONG__ 4")
  message(FATAL_ERROR "Windows native_sim requires i686 MinGW with four-byte pointers and long.")
endif()
if(DEFINED NATIVE_TARGET_HOST AND NOT NATIVE_TARGET_HOST STREQUAL "x86_64")
  message(FATAL_ERROR "Windows native_sim requires the x86 target family, not ${NATIVE_TARGET_HOST}.")
endif()
if(NOT CONFIG_NATIVE_SIMULATOR_NUMBER_MCUS EQUAL 1 OR CONFIG_NATIVE_SIMULATOR_EXTRA_IMAGE_PATHS)
  message(FATAL_ERROR "Windows native_sim supports one embedded image.")
endif()
if(CONFIG_NATIVE_SIMULATOR_NCE_BACKEND_COROUTINES AND
   NOT CONFIG_NATIVE_SIMULATOR_NCT_BACKEND_COROUTINES)
  message(FATAL_ERROR "Windows coroutine NCE requires coroutine NCT.")
endif()
if(CONFIG_NATIVE_SIMULATOR_NCT_BACKEND_COROUTINES AND
   NOT CONFIG_NATIVE_SIMULATOR_COROUTINE_IMPLEMENTATION_WINDOWS_FIBERS AND
   NOT CONFIG_NATIVE_SIMULATOR_COROUTINE_IMPLEMENTATION_ASM AND
   NOT CONFIG_NATIVE_SIMULATOR_COROUTINE_IMPLEMENTATION_BOOST_CONTEXT)
  message(FATAL_ERROR "Windows coroutine NCT supports Fibers, the assembly switch and "
                      "Boost.Context. The UCRT has no ucontext functions.")
endif()
if(CONFIG_NATIVE_SIMULATOR_NCT_BACKEND_COROUTINES AND
   (CONFIG_THREAD_ANALYZER OR CONFIG_THREAD_RUNTIME_STACK_SAFETY))
  message(FATAL_ERROR "Windows coroutine NCT does not support native unused-stack accounting.")
endif()
if(NOT CONFIG_EXTERNAL_LIBC)
  message(FATAL_ERROR "Experimental Windows native_sim requires CONFIG_EXTERNAL_LIBC=y. "
                     "The selected MinGW compiler headers depend on its host C runtime.")
endif()

foreach(feature
    ASAN AUDIO_DMIC_NATIVE_SIM BT_USERCHAN CAN_NATIVE_LINUX COVERAGE CPP
    EEPROM_SIMULATOR ETH_NATIVE_TAP FILE_SYSTEM_NATIVE_MOUNT
    FLASH_SIMULATOR FUSE_FS_ACCESS GPROF HAS_SDL I2S_NATIVE_SIM LLEXT LTO
    MODEM_BACKEND_TTY MSAN NATIVE_LINUX_EVDEV NATIVE_LINUX_LEDS NATIVE_LINUX_TEMP
    NATIVE_SIM_REBOOT NATIVE_SIM_SLOWDOWN_TO_REAL_TIME NET_NATIVE_OFFLOADED_SOCKETS
    PTP_CLOCK_NATIVE REBOOT RTC_EMUL TRACING_BACKEND_POSIX UART_NATIVE_PTY UART_NATIVE_TTY
    UBSAN USB_NATIVE_POSIX VIDEO_NATIVE_SIM_FIFO WIFI_NATIVE_SIM ARCH_POSIX_LIBFUZZER
)
  if(CONFIG_${feature})
    message(FATAL_ERROR "Experimental Windows native_sim does not support CONFIG_${feature}. "
                       "Disable it explicitly; board defaults are not changed automatically.")
  endif()
endforeach()
foreach(path "${ZEPHYR_BASE}" "${APPLICATION_BINARY_DIR}" "${CMAKE_C_COMPILER}")
  if(path MATCHES "[ \t\r\n]")
    message(FATAL_ERROR "Windows native_sim currently requires paths without whitespace: ${path}")
  endif()
endforeach()

# COFF relocatable GC can discard live weak fallbacks. The runner performs final GC
# after weak metadata repair and localization, retaining normal archive selection.
get_property(windows_link_base TARGET linker PROPERTY base)
list(REMOVE_ITEM windows_link_base "${LINKERFLAGPREFIX},--gc-sections")
set_property(TARGET linker PROPERTY base "${windows_link_base}")

# Inspect COFF headers through the existing post-link statistics recipe.
set_property(TARGET bintools PROPERTY readelf_command ${CMAKE_OBJDUMP})
set_property(TARGET bintools PROPERTY readelf_flag_headers -x)

# Clang writes an address-significance table for LLD in every object, marked for
# removal. GNU ld does not use it, and its relocatable link drops that mark, so the
# final link would place the table at address 0 of the image.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
  zephyr_compile_options(-fno-addrsig)
endif()

# GCC leaves the start files and default libraries out of a relocatable link, but
# Clang's MinGW driver adds them. The runner links them into the final image.
set_property(TARGET linker PROPERTY partial_linking -r -nostdlib)

# PE has no ELF hidden visibility or PIE mode. Embedded code remains relocatable.
set_property(TARGET linker PROPERTY no_position_independent "")
