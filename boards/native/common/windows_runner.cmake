# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

# Export only the runner interface, not Zephyr's directory flags or include paths.
set(nsi_runner_dir "${APPLICATION_BINARY_DIR}/zephyr/NSI")
set(nsi_runner_build "${nsi_runner_dir}/build")
set(nsi_runner_config "${nsi_runner_dir}/runner-config.cmake")
set(nsi_runner_content "")
macro(nsi_runner_export name value)
  string(APPEND nsi_runner_content "set(${name} [====[${value}]====])\n")
endmacro()

foreach(tool C_COMPILER C_COMPILER_LAUNCHER AR RANLIB OBJCOPY MAKE_PROGRAM)
  nsi_runner_export(CMAKE_${tool} "${CMAKE_${tool}}")
endforeach()
nsi_runner_export(NSI_DIR "${NSI_DIR}")
nsi_runner_export(NSI_PYTHON "${PYTHON_EXECUTABLE}")
nsi_runner_export(NSI_COFF_HELPER "${ZEPHYR_BASE}/scripts/build/localize_coff.py")
nsi_runner_export(NSI_EMBEDDED "${PROJECT_BINARY_DIR}/${KERNEL_ELF_NAME}")
nsi_runner_export(NSI_EXE "${PROJECT_BINARY_DIR}/${KERNEL_EXE_NAME}")
nsi_runner_export(NSI_BUILD_EXE "${CONFIG_BUILD_OUTPUT_EXE}")

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

if(CONFIG_BUILD_OUTPUT_EXE)
  add_custom_target(native_runner_executable ALL
    COMMAND ${nsi_runner_configure} -DNSI_LINK_INPUTS_READY=ON
    COMMAND "${CMAKE_COMMAND}" --build "${nsi_runner_build}" --target link_with_esw
    DEPENDS runner_lib ${logical_target_for_zephyr_elf} ${nsi_runner_dependencies}
      $<TARGET_GENEX_EVAL:native_simulator,${nsi_runner_library_files}>
    BYPRODUCTS "${PROJECT_BINARY_DIR}/${KERNEL_EXE_NAME}"
    VERBATIM
    USES_TERMINAL
  )
  set(BYPRODUCT_KERNEL_EXE_NAME "${PROJECT_BINARY_DIR}/${KERNEL_EXE_NAME}"
    CACHE FILEPATH "Kernel exe file" FORCE
  )
endif()
