
set(TOOLCHAIN_VARIANT_COMPILER gnu CACHE STRING "Variant compiler being used")
set(COMPILER host-gcc)
if(CMAKE_HOST_APPLE)
  set(LINKER darwin)
  set(CMAKE_READELF ${ZEPHYR_BASE}/scripts/build/macho_readelf.py
      CACHE FILEPATH "Mach-O metadata tool" FORCE)
else()
  set(LINKER ld)
endif()
set(BINTOOLS host-gnu)

set(TOOLCHAIN_HAS_NEWLIB OFF CACHE BOOL "True if toolchain supports newlib")

if(CMAKE_HOST_APPLE)
  message(STATUS "Found toolchain: host (Apple Clang/ld64)")
else()
  message(STATUS "Found toolchain: host (gcc/ld)")
endif()
