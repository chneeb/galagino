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

add_executable(galagino_pizero
    main.cpp
    pad.c
    usb_input.c
    ram_wrappers.c
    hidparser/hidparser.c
    ${COMMON}/galagino_core.c
    ${GALAGINO}/Z80.c
    ${GALAGINO}/i8048.c
    ${GALAGINO}/emulation.c
)

target_include_directories(galagino_pizero PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${COMMON}
    ${GALAGINO}
    ${CMAKE_CURRENT_LIST_DIR}/pico_lib
)

# pizero_config.h defines _CONFIG_H_, which turns the CYD config.h in
# ../galagino into a no-op wherever it is included.
target_compile_options(galagino_pizero PRIVATE
    -include ${CMAKE_CURRENT_LIST_DIR}/pizero_config.h
    -O2
)

target_compile_definitions(galagino_pizero PRIVATE
    PICO_PIO_USE_GPIO_BASE=1     # TMDS pins are GPIO 32-38
    CFG_TUSB_CONFIG_FILE="tusb_config.h"
    DVI_N_BUFFERS=8              # slack for 480 unique lines (pico_lib default 5)
)

target_link_libraries(galagino_pizero PRIVATE
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

# Core 1's DVI path must not fetch code from flash (see ram_wrappers.c):
# redirect the flash-resident library functions it calls to RAM copies.
target_link_options(galagino_pizero PRIVATE
    -Wl,--wrap=memset
    -Wl,--wrap=memcpy
    -Wl,--wrap=interp_save
    -Wl,--wrap=interp_restore
)
set_source_files_properties(ram_wrappers.c PROPERTIES
    COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns"
)
# ...and keep GCC from turning core 1's fill loops into memset calls
set_source_files_properties(main.cpp PROPERTIES
    COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns"
)

# The native USB port is the gamepad host, so stdio goes to UART0 (TX GP0).
pico_enable_stdio_usb(galagino_pizero 0)
pico_enable_stdio_uart(galagino_pizero 1)

pico_add_extra_outputs(galagino_pizero)
