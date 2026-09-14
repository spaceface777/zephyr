# Copyright (c) 2023 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

set(zephyr_build_path ${APPLICATION_BINARY_DIR}/zephyr)
get_property(CCACHE GLOBAL PROPERTY RULE_LAUNCH_COMPILE)

if(NOT CMAKE_HOST_APPLE)
  target_link_options(native_simulator INTERFACE
    "-T ${ZEPHYR_BASE}/boards/native/common/natsim_linker_script.ld")
endif()

if(SYSROOT_DIR)
  message(NOTICE "Appending --sysroot=${SYSROOT_DIR} to native_simulator")
  target_compile_options(native_simulator INTERFACE "--sysroot=${SYSROOT_DIR}")
  target_link_options(native_simulator INTERFACE "--sysroot=${SYSROOT_DIR}")
endif()

if(CONFIG_NATIVE_SIMULATOR_STATIC_LINKING)
  target_link_options(native_simulator INTERFACE "-static")
endif()

if("${LINKER}" STREQUAL "lld")
  target_link_options(native_simulator INTERFACE "-fuse-ld=lld")
endif()

file(GLOB nsi_runner_core_sources CONFIGURE_DEPENDS "${NSI_DIR}/common/src/*.c")
file(GLOB nsi_runner_native_sources CONFIGURE_DEPENDS "${NSI_DIR}/native/src/*.c")
# Applications can replace runner components without patching their sources.
# Generator expressions permit overrides after find_package(Zephyr).
set_property(TARGET native_simulator PROPERTY RUNNER_CORE_SOURCES "${nsi_runner_core_sources}")
set_property(TARGET native_simulator PROPERTY RUNNER_NATIVE_SOURCES "${nsi_runner_native_sources}")

set(nsi_config_content
  ${nsi_config_content}
  "NSI_AR:=${CMAKE_AR}"
  "NSI_BUILD_OPTIONS:=$<JOIN:$<TARGET_PROPERTY:native_simulator,INTERFACE_COMPILE_OPTIONS>,\ >"
  "NSI_BUILD_PATH:=${zephyr_build_path}/NSI"
  "NSI_CC:=${CCACHE} ${CMAKE_C_COMPILER}"
  "NSI_LINKER:=${CMAKE_C_COMPILER}"
  "NSI_NM:=${CMAKE_NM}"
  "NSI_OBJCOPY:=${CMAKE_OBJCOPY}"
  "NSI_PYTHON:=${PYTHON_EXECUTABLE}"
  "NSI_MACHO_LINK:=${ZEPHYR_BASE}/scripts/build/macho_link.py"
  "NSI_EMBEDDED_CPU_SW:=${zephyr_build_path}/${KERNEL_ELF_NAME} ${CONFIG_NATIVE_SIMULATOR_EXTRA_IMAGE_PATHS}"
  "NSI_EXE:=${zephyr_build_path}/${KERNEL_EXE_NAME}"
  "NSI_EXTRA_SRCS:=$<JOIN:$<TARGET_PROPERTY:native_simulator,INTERFACE_SOURCES>,\ >"
  "NSI_LINK_OPTIONS:=$<JOIN:$<TARGET_PROPERTY:native_simulator,INTERFACE_LINK_OPTIONS>,\ >"
  "NSI_EXTRA_LIBS:=$<JOIN:$<TARGET_PROPERTY:native_simulator,RUNNER_LINK_LIBRARIES>,\ >"
  "NSI_PATH:=${NSI_DIR}/"
  "NSI_N_CPUS:=${CONFIG_NATIVE_SIMULATOR_NUMBER_MCUS}"
  "NSI_RUNNER_CORE_SRCS:=$<JOIN:$<TARGET_PROPERTY:native_simulator,RUNNER_CORE_SOURCES>,\ >"
  "NSI_RUNNER_NATIVE_SRCS:=$<JOIN:$<TARGET_PROPERTY:native_simulator,RUNNER_NATIVE_SOURCES>,\ >"
  "NSI_LOCALIZE_OPTIONS:=--localize-symbol=CONFIG_* $<JOIN:$<TARGET_PROPERTY:native_simulator,LOCALIZE_EXTRA_OPTIONS>,\ >"
)

if(CMAKE_HOST_APPLE)
  list(APPEND nsi_config_content "NSI_MACHO:=1")
endif()

string(REPLACE ";" "\n" nsi_config_content "${nsi_config_content}")

file(GENERATE OUTPUT "${zephyr_build_path}/NSI/nsi_config"
  CONTENT "${nsi_config_content}"
)
