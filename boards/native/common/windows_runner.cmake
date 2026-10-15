# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

option(NATIVE_SIM_FIBER_SESSION_EXTERNAL "Use a host-owned Fibers session provider" OFF)
if(NATIVE_SIM_FIBER_SESSION_EXTERNAL AND CONFIG_BUILD_OUTPUT_EXE)
  message(FATAL_ERROR "External Fibers session provider requires CONFIG_BUILD_OUTPUT_EXE=n.")
endif()

# Export only the runner interface, not Zephyr's directory flags or include paths.
set(nsi_runner_dir "${APPLICATION_BINARY_DIR}/zephyr/NSI")
set(nsi_runner_build "${nsi_runner_dir}/build")
set(nsi_runner_config "${nsi_runner_dir}/runner-config.cmake")
set(nsi_runner_content "")
function(nsi_runner_export name value)
  # Evaluate values into data files, never into CMake source code.
  # An explicit trailing newline makes native newline conversion deterministic.
  file(GENERATE OUTPUT "${nsi_runner_dir}/${name}.txt" CONTENT "${value}\n"
    NEWLINE_STYLE UNIX
    CONDITION "$<COMPILE_LANGUAGE:C>"
    TARGET native_simulator
  )
  string(APPEND nsi_runner_content
    "file(READ \"\${CMAKE_CURRENT_LIST_DIR}/${name}.txt\" ${name})\n"
    "string(LENGTH \"\${${name}}\" nsi_runner_value_length)\n"
    "math(EXPR nsi_runner_value_length \"\${nsi_runner_value_length} - 1\")\n"
    "string(SUBSTRING \"\${${name}}\" 0 \${nsi_runner_value_length} ${name})\n"
    "set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "
    "\"\${CMAKE_CURRENT_LIST_DIR}/${name}.txt\")\n"
  )
  set(nsi_runner_content "${nsi_runner_content}" PARENT_SCOPE)
endfunction()

foreach(tool C_COMPILER C_COMPILER_LAUNCHER CXX_COMPILER CXX_COMPILER_LAUNCHER AR RANLIB OBJCOPY
    MAKE_PROGRAM)
  nsi_runner_export(CMAKE_${tool} "${CMAKE_${tool}}")
endforeach()
nsi_runner_export(NSI_DIR "${NSI_DIR}")
nsi_runner_export(NSI_PYTHON "${PYTHON_EXECUTABLE}")
nsi_runner_export(NSI_COFF_HELPER "${ZEPHYR_BASE}/scripts/build/localize_coff.py")
nsi_runner_export(NSI_EMBEDDED "${PROJECT_BINARY_DIR}/${KERNEL_ELF_NAME}")
nsi_runner_export(NSI_EXE "${PROJECT_BINARY_DIR}/${KERNEL_EXE_NAME}")
nsi_runner_export(NSI_BUILD_EXE "${CONFIG_BUILD_OUTPUT_EXE}")
nsi_runner_export(NSI_FIBER_SESSION_EXTERNAL "${NATIVE_SIM_FIBER_SESSION_EXTERNAL}")
nsi_runner_export(NSI_NCE_BACKEND
  "$<IF:$<BOOL:${CONFIG_NATIVE_SIMULATOR_NCE_BACKEND_COROUTINES}>,coroutines,pthreads>")
nsi_runner_export(NSI_NCT_BACKEND
  "$<IF:$<BOOL:${CONFIG_NATIVE_SIMULATOR_NCT_BACKEND_COROUTINES}>,coroutines,pthreads>")
set(nsi_coro_backend unsupported)
if(CONFIG_NATIVE_SIMULATOR_COROUTINE_IMPLEMENTATION_WINDOWS_FIBERS)
  set(nsi_coro_backend fibers)
elseif(CONFIG_NATIVE_SIMULATOR_COROUTINE_IMPLEMENTATION_BOOST_CONTEXT)
  set(nsi_coro_backend boost)
elseif(CONFIG_NATIVE_SIMULATOR_COROUTINE_IMPLEMENTATION_ASM)
  set(nsi_coro_backend asm)
endif()
nsi_runner_export(NSI_CORO_BACKEND "${nsi_coro_backend}")

# The C++ Boost.Context primitive needs the headers and the static library.
set(nsi_boost_include_dirs "")
set(nsi_boost_context_lib "")
if(CONFIG_NATIVE_SIMULATOR_NCT_BACKEND_COROUTINES AND
   CONFIG_NATIVE_SIMULATOR_COROUTINE_IMPLEMENTATION_BOOST_CONTEXT)
  # The runner interface is evaluated in the scope of another directory.
  find_package(Boost CONFIG REQUIRED COMPONENTS context GLOBAL)
  set(nsi_boost_include_dirs "$<TARGET_PROPERTY:Boost::context,INTERFACE_INCLUDE_DIRECTORIES>")
  set(nsi_boost_context_lib "$<TARGET_FILE:Boost::context>")
endif()
nsi_runner_export(NSI_BOOST_INCLUDE_DIRS "${nsi_boost_include_dirs}")
nsi_runner_export(NSI_BOOST_CONTEXT_LIB "${nsi_boost_context_lib}")

foreach(property
    INTERFACE_SOURCES
    INTERFACE_INCLUDE_DIRECTORIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
    INTERFACE_COMPILE_DEFINITIONS
    INTERFACE_COMPILE_OPTIONS
    INTERFACE_LINK_OPTIONS
)
  nsi_runner_export(NSI_${property}
    "$<TARGET_GENEX_EVAL:native_simulator,$<TARGET_PROPERTY:native_simulator,${property}>>")
endforeach()

# Resolve CMake library targets with the tools and build graph selected by Zephyr.
get_target_property(nsi_runner_libraries native_simulator RUNNER_LINK_LIBRARIES)
set(nsi_runner_library_files "")
set(nsi_runner_dependencies "")
foreach(library IN LISTS nsi_runner_libraries)
  if(TARGET "${library}")
    list(APPEND nsi_runner_library_files "$<TARGET_FILE:${library}>")
    list(APPEND nsi_runner_dependencies "${library}")
  else()
    list(APPEND nsi_runner_library_files "${library}")
  endif()
endforeach()
nsi_runner_export(NSI_LIBRARIES
  "$<TARGET_GENEX_EVAL:native_simulator,${nsi_runner_library_files}>")
file(GENERATE OUTPUT "${nsi_runner_config}" CONTENT "${nsi_runner_content}"
  CONDITION "$<COMPILE_LANGUAGE:C>"
  TARGET native_simulator
)

if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
  message(FATAL_ERROR "Windows native_sim requires the Ninja generator.")
endif()
set(nsi_runner_configure
  "${CMAKE_COMMAND}" -S "${NSI_DIR}/windows" -B "${nsi_runner_build}" -G Ninja
  "-DNSI_CONFIG=${nsi_runner_config}"
)
# Always enter the nested build so its compiler depfiles can detect changed headers.
# Its native CMake targets decide which compilation and link commands need to run.
add_custom_target(runner_lib
  COMMAND ${nsi_runner_configure} -DNSI_LINK_INPUTS_READY=OFF
  COMMAND "${CMAKE_COMMAND}" --build "${nsi_runner_build}" --target runner_lib
  DEPENDS
    $<TARGET_GENEX_EVAL:native_simulator,$<TARGET_PROPERTY:native_simulator,INTERFACE_SOURCES>>
  BYPRODUCTS "${nsi_runner_build}/runner.a"
  VERBATIM
  USES_TERMINAL
)
add_dependencies(runner_lib native_simulator)
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${nsi_runner_build}")

# Build the CPU object and layout for custom image links without linking an executable.
add_custom_target(native_runner_link_inputs
  COMMAND ${nsi_runner_configure} -DNSI_LINK_INPUTS_READY=ON
  COMMAND "${CMAKE_COMMAND}" --build "${nsi_runner_build}" --target link_inputs
  DEPENDS runner_lib ${logical_target_for_zephyr_elf} ${nsi_runner_dependencies}
    $<TARGET_GENEX_EVAL:native_simulator,${nsi_runner_library_files}>
  BYPRODUCTS
    "${nsi_runner_build}/embedded.loc_cpusw.o"
    "${nsi_runner_build}/embedded.loc_cpusw.o.ld"
    "${nsi_runner_build}/linker_script.ld"
    "${nsi_runner_build}/windows_linker_script.ld"
  VERBATIM
  USES_TERMINAL
)

if(CONFIG_BUILD_OUTPUT_EXE)
  add_custom_target(native_runner_executable ALL
    COMMAND "${CMAKE_COMMAND}" --build "${nsi_runner_build}" --target link_with_esw
    DEPENDS native_runner_link_inputs
    BYPRODUCTS "${PROJECT_BINARY_DIR}/${KERNEL_EXE_NAME}"
    VERBATIM
    USES_TERMINAL
  )
  set(BYPRODUCT_KERNEL_EXE_NAME "${PROJECT_BINARY_DIR}/${KERNEL_EXE_NAME}"
    CACHE FILEPATH "Kernel exe file" FORCE
  )
endif()
