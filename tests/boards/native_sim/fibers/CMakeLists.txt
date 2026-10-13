# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fibers_state C)
target_sources(app PRIVATE main.c)
if(CONFIG_NATIVE_SIMULATOR_COROUTINE_IMPLEMENTATION_WINDOWS_FIBERS)
  target_sources(native_simulator INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/host.c)
  target_compile_definitions(native_simulator INTERFACE
    $<$<BOOL:${CONFIG_TEST_FIBER_BORROWED_ROOT}>:TEST_FIBER_BORROWED_ROOT>
  )
endif()
