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

# pico_lib (MIT), vendored from msx2pico with local patches, see README.md
add_subdirectory(pico_lib/dvi)
add_subdirectory(pico_lib/util)

foreach(target proto_480 proto_doubled proto_480_direct)
  add_executable(${target} main.cpp)
  target_include_directories(${target} PRIVATE pico_lib)
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

target_compile_definitions(proto_doubled PRIVATE PROTO_DOUBLED=1)
target_compile_definitions(proto_480_direct PRIVATE PROTO_DIRECT=1)
