cmake_minimum_required(VERSION 3.13)

# DVI geometry prototype for Galagino on the Waveshare RP2350-PiZero.
# Builds proto_480 (480 unique lines, WIDE/BLEND alternating), proto_doubled
# (upstream pico_lib line doubling) and proto_480_direct (480 lines, encoder only).

set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350 CACHE STRING "Platform")

include(pico_sdk_import.cmake)

project(pizero_dvi_proto C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# pico_lib (MIT), vendored in ../galagino_pizero with local patches
set(PICO_LIB ${CMAKE_CURRENT_LIST_DIR}/../galagino_pizero/pico_lib)
add_subdirectory(${PICO_LIB}/dvi pico_lib_dvi)
add_subdirectory(${PICO_LIB}/util pico_lib_util)

foreach(target proto_480 proto_doubled proto_480_direct)
  add_executable(${target} main.cpp ../galagino_pizero/ram_wrappers.c)
  target_include_directories(${target} PRIVATE ${PICO_LIB})
  # core 1 must not run flash code: RAM copies of memset/memcpy/interp_*
  target_link_options(${target} PRIVATE
      -Wl,--wrap=memset -Wl,--wrap=memcpy
      -Wl,--wrap=interp_save -Wl,--wrap=interp_restore)
  target_compile_definitions(${target} PRIVATE
      PICO_PIO_USE_GPIO_BASE=1     # TMDS pins are GPIO 32-38
  )
  target_compile_options(${target} PRIVATE -O2)
  target_link_libraries(${target} PRIVATE
      pico_stdlib
      pico_multicore
      hardware_dma
      hardware_pio
      hardware_interp
      hardware_vreg
      hardware_clocks
      dvi
      util
  )
  pico_enable_stdio_usb(${target} 1)
  pico_enable_stdio_uart(${target} 0)
  pico_add_extra_outputs(${target})
endforeach()

set_source_files_properties(../galagino_pizero/ram_wrappers.c PROPERTIES
    COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns")

target_compile_definitions(proto_doubled PRIVATE PROTO_DOUBLED=1)
target_compile_definitions(proto_480_direct PRIVATE PROTO_DIRECT=1 DVI_N_BUFFERS=8)
target_compile_definitions(proto_480 PRIVATE DVI_N_BUFFERS=8)
