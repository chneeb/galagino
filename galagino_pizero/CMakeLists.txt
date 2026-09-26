cmake_minimum_required(VERSION 3.13)

# Galagino for the Waveshare RP2350-PiZero with DVI/HDMI output.
# Builds the unchanged emulation sources from ../galagino, the shared
# RP2350 code from ../galagino_pico_common and a DVI platform layer.

set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350 CACHE STRING "Platform")

include(pico_sdk_import.cmake)

project(galagino_pizero C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Off: flash keeps the bootrom's timing, as msx2pico does on this board.
# On: embed the SDK's W25Q080 XIP setup at clk_sys/4 = 63 MHz, like the
# PicoCalc build. Try it if emulation is slower than the PicoCalc numbers.
option(GALAGINO_FAST_FLASH "Embed QSPI XIP setup (clk_sys/4)" OFF)
if(GALAGINO_FAST_FLASH)
  add_compile_definitions(PICO_FLASH_SPI_CLKDIV=4 PICO_EMBED_XIP_SETUP=1)
endif()

pico_sdk_init()

set(GALAGINO ${CMAKE_CURRENT_LIST_DIR}/../galagino)
set(COMMON ${CMAKE_CURRENT_LIST_DIR}/../galagino_pico_common)

# pico_lib (MIT), vendored from msx2pico with local patches, see README.md
add_subdirectory(pico_lib/dvi)
add_subdirectory(pico_lib/util)

# emulation.c calls a few Arduino/FreeRTOS functions; map them to the SDK.
set_source_files_properties(${GALAGINO}/emulation.c PROPERTIES
    COMPILE_OPTIONS "-include;${COMMON}/esp32_compat.h"
)

# galagino_pizero:        USB gamepads on the PIO-USB port, serial console on USB-C
# galagino_pizero_native: USB gamepads on the native USB-C port (like frank-snes),
#                         no serial console; use the on-screen diagnostics
function(galagino_pizero_target target native)
  add_executable(${target}
      main.cpp
      pad.c
      usb_input.c
      hidparser/hidparser.c
      ${COMMON}/galagino_core.c
      ${GALAGINO}/Z80.c
      ${GALAGINO}/i8048.c
      ${GALAGINO}/emulation.c
  )
  if(NOT native)
    target_sources(${target} PRIVATE usb_descriptors.c)
  endif()

  target_include_directories(${target} PRIVATE
      ${CMAKE_CURRENT_LIST_DIR}
      ${COMMON}
      ${GALAGINO}
      ${CMAKE_CURRENT_LIST_DIR}/pico_lib
  )

  # pizero_config.h defines _CONFIG_H_, which turns the CYD config.h in
  # ../galagino into a no-op wherever it is included.
  target_compile_options(${target} PRIVATE
      -include ${CMAKE_CURRENT_LIST_DIR}/pizero_config.h
      -O2
  )

  target_compile_definitions(${target} PRIVATE
      PICO_PIO_USE_GPIO_BASE=1     # TMDS pins are GPIO 32-38
      PICO_DEFAULT_PIO_USB_DP_PIN=28
      CFG_TUSB_CONFIG_FILE="tusb_config.h"
      GALAGINO_USB_NATIVE=$<BOOL:${native}>
  )

  target_link_libraries(${target} PRIVATE
      pico_stdlib
      pico_multicore
      hardware_dma
      hardware_pio
      hardware_interp
      hardware_i2c
      hardware_vreg
      hardware_clocks
      hardware_watchdog
      pico_rand
      dvi
      util
      tinyusb_host
  )
  if(NOT native)
    target_link_libraries(${target} PRIVATE tinyusb_device tinyusb_pico_pio_usb)
  endif()

  # The SDK disables its USB stdio when the TinyUSB host is linked;
  # usb_input.c provides the USB serial console instead (PIO-USB build).
  # UART0 (GP0) as backup in both.
  pico_enable_stdio_usb(${target} 0)
  pico_enable_stdio_uart(${target} 1)

  pico_add_extra_outputs(${target})
endfunction()

galagino_pizero_target(galagino_pizero FALSE)
galagino_pizero_target(galagino_pizero_native TRUE)
