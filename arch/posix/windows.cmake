# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

# Native Windows only, with the selected i686 MinGW compiler and host C runtime.
if(NOT NATIVE_SIM_EXPERIMENTAL_WINDOWS)
  message(FATAL_ERROR "Windows native_sim requires NATIVE_SIM_EXPERIMENTAL_WINDOWS=ON.")
endif()
if(NOT CONFIG_BOARD_NATIVE_SIM OR CONFIG_64BIT)
  message(FATAL_ERROR "Experimental Windows support requires native_sim/native (32 bit).")
endif()
if(NOT ZEPHYR_TOOLCHAIN_VARIANT STREQUAL "host" OR
   NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "Windows native_sim requires the host toolchain with i686 MinGW GCC.")
endif()

execute_process(
  COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
  OUTPUT_VARIABLE windows_triplet
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
  COMMAND "${CMAKE_C_COMPILER}" -dM -E -x c "${ZEPHYR_BASE}/misc/empty_file.c"
  OUTPUT_VARIABLE windows_macros
  COMMAND_ERROR_IS_FATAL ANY
)
if(NOT windows_triplet STREQUAL "i686-w64-mingw32" OR
   NOT windows_macros MATCHES "#define _WIN32 1" OR
   windows_macros MATCHES "#define _WIN64" OR
   NOT windows_macros MATCHES "#define __SIZEOF_POINTER__ 4" OR
   NOT windows_macros MATCHES "#define __SIZEOF_LONG__ 4")
  message(FATAL_ERROR "Windows native_sim requires i686 MinGW with four-byte pointers and long.")
endif()
if(DEFINED NATIVE_TARGET_HOST AND NOT NATIVE_TARGET_HOST STREQUAL "x86_64")
  message(FATAL_ERROR "Windows native_sim requires the x86 target family, not ${NATIVE_TARGET_HOST}.")
endif()
# The native simulator uses pthread NCE and NCT implementations.
if(NOT CONFIG_NATIVE_SIMULATOR_NUMBER_MCUS EQUAL 1 OR
   CONFIG_NATIVE_SIMULATOR_EXTRA_IMAGE_PATHS)
  message(FATAL_ERROR "Windows native_sim supports one embedded image with pthread NCE and NCT.")
endif()
if(NOT CONFIG_EXTERNAL_LIBC)
  message(FATAL_ERROR "Experimental Windows native_sim requires CONFIG_EXTERNAL_LIBC=y. "
                     "The selected MinGW compiler headers depend on its host C runtime.")
endif()

foreach(feature
    ASAN AUDIO_DMIC_NATIVE_SIM BT_USERCHAN CAN_NATIVE_LINUX COVERAGE CPP
    EEPROM_SIMULATOR ENTROPY_GENERATOR ETH_NATIVE_TAP FILE_SYSTEM_NATIVE_MOUNT
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

# PE has no ELF hidden visibility or PIE mode. Embedded code remains relocatable.
set_property(TARGET linker PROPERTY no_position_independent "")
