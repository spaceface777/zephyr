# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

# LLD's COFF linker for Windows native_sim images.
#
# It takes neither a linker script nor -r. The relocatable link of the embedded
# software is replaced by scripts/build/coff_layout.py, which applies the linker
# script by renaming input sections into grouped COFF sections that LLD orders by
# name, and records the laid out inputs for the native simulator runner. It also
# links the kernel image from them, for the steps that analyse it.

macro(toolchain_linker_finalize)
  set(coff_layout
    "\"${PYTHON_EXECUTABLE}\" \"${ZEPHYR_BASE}/scripts/build/coff_layout.py\" layout")
  if(CMAKE_C_COMPILER_TARGET)
    string(APPEND coff_layout " --image-option=--target=${CMAKE_C_COMPILER_TARGET}")
  endif()
  set(common_link "<FLAGS> <LINK_FLAGS> <OBJECTS> <LINK_LIBRARIES>")
  foreach(language C ASM CXX)
    set(CMAKE_${language}_LINK_EXECUTABLE
      "${coff_layout} --output <TARGET> -- <CMAKE_${language}_COMPILER> ${common_link}")
  endforeach()
endmacro()
