# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(windows_coroutines C)
target_sources(app PRIVATE main.c)
if(CMAKE_HOST_WIN32 AND CONFIG_NATIVE_SIMULATOR_NCT_BACKEND_COROUTINES AND
   NOT CONFIG_NATIVE_SIMULATOR_COROUTINE_IMPLEMENTATION_WINDOWS_FIBERS)
  target_compile_definitions(app PRIVATE TEST_WINDOWS_COROUTINES)
  target_sources(native_simulator INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/host.c)
endif()
