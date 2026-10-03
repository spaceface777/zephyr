# Copyright (c) 2023 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

set(zephyr_build_path ${APPLICATION_BINARY_DIR}/zephyr)

target_link_options(native_simulator INTERFACE
  "-T ${ZEPHYR_BASE}/boards/native/common/natsim_linker_script.ld")

if(SYSROOT_DIR)
  message(NOTICE "Appending --sysroot=${SYSROOT_DIR} to native_simulator")
  target_compile_options(native_simulator INTERFACE "--sysroot=${SYSROOT_DIR}")
  target_link_options(native_simulator INTERFACE "--sysroot=${SYSROOT_DIR}")
endif()

if(CONFIG_NATIVE_SIMULATOR_STATIC_LINKING)
  target_link_options(native_simulator INTERFACE "-static")
endif()

set(nsi_boost_context_includes "")
set(nsi_boost_context_lib "")
if(CONFIG_NATIVE_SIMULATOR_COROUTINE_IMPLEMENTATION_BOOST_CONTEXT)
  find_package(Boost REQUIRED COMPONENTS context)
  set(nsi_boost_context_include_list ${Boost_INCLUDE_DIRS})
  list(TRANSFORM nsi_boost_context_include_list PREPEND "-I")
  list(JOIN nsi_boost_context_include_list " " nsi_boost_context_includes)
  set(nsi_boost_context_lib "$<TARGET_FILE:Boost::context>")
endif()

list(JOIN CMAKE_C_COMPILER_LAUNCHER " " launcher)

if("${LINKER}" STREQUAL "lld")
  target_link_options(native_simulator INTERFACE "-fuse-ld=lld")
endif()

set(nsi_config_content
  ${nsi_config_content}
  "NSI_AR:=${CMAKE_AR}"
  "NSI_BUILD_OPTIONS:=$<JOIN:$<TARGET_PROPERTY:native_simulator,INTERFACE_COMPILE_OPTIONS>,\ >"
  "NSI_BUILD_PATH:=${zephyr_build_path}/NSI"
  "NSI_CC:=$<$<BOOL:${launcher}>:${launcher} >${CMAKE_C_COMPILER}"
  "NSI_CXX:=$<$<BOOL:${launcher}>:${launcher} >${CMAKE_C_COMPILER}"
  "NSI_OBJCOPY:=${CMAKE_OBJCOPY}"
  "NSI_EMBEDDED_CPU_SW:=${zephyr_build_path}/${KERNEL_ELF_NAME} ${CONFIG_NATIVE_SIMULATOR_EXTRA_IMAGE_PATHS}"
  "NSI_EXE:=${zephyr_build_path}/${KERNEL_EXE_NAME}"
  "NSI_EXTRA_SRCS:=$<JOIN:$<TARGET_PROPERTY:native_simulator,INTERFACE_SOURCES>,\ >"
  "NSI_LINK_OPTIONS:=$<JOIN:$<TARGET_PROPERTY:native_simulator,INTERFACE_LINK_OPTIONS>,\ >"
  "NSI_EXTRA_LIBS:=$<JOIN:$<TARGET_PROPERTY:native_simulator,RUNNER_LINK_LIBRARIES>,\ >"
  "NSI_PATH:=${NSI_DIR}/"
  "NSI_N_CPUS:=${CONFIG_NATIVE_SIMULATOR_NUMBER_MCUS}"
  "NSI_NCE_BACKEND:=$<IF:$<BOOL:${CONFIG_NATIVE_SIMULATOR_NCE_BACKEND_COROUTINES}>,coroutines,pthreads>"
  "NSI_NCT_BACKEND:=$<IF:$<BOOL:${CONFIG_NATIVE_SIMULATOR_NCT_BACKEND_COROUTINES}>,coroutines,pthreads>"
  "NSI_CORO_BACKEND:=$<IF:$<BOOL:${CONFIG_NATIVE_SIMULATOR_COROUTINE_IMPLEMENTATION_BOOST_CONTEXT}>,boost,$<IF:$<BOOL:${CONFIG_NATIVE_SIMULATOR_COROUTINE_IMPLEMENTATION_UCONTEXT}>,ucontext,asm>>"
  "NSI_BOOST_CONTEXT_INCLUDES:=${nsi_boost_context_includes}"
  "NSI_BOOST_CONTEXT_LIB:=${nsi_boost_context_lib}"
  "NSI_LOCALIZE_OPTIONS:=--localize-symbol=CONFIG_* $<JOIN:$<TARGET_PROPERTY:native_simulator,LOCALIZE_EXTRA_OPTIONS>,\ >"
)

string(REPLACE ";" "\n" nsi_config_content "${nsi_config_content}")

file(GENERATE OUTPUT "${zephyr_build_path}/NSI/nsi_config"
  CONTENT "${nsi_config_content}"
)
