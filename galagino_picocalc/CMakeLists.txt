cmake_minimum_required(VERSION 3.13)

# Galagino for the ClockworkPi PicoCalc (Pico 2 / RP2350).
# Builds the unchanged emulation sources from ../galagino with a PicoCalc
# platform layer (PIO SPI LCD, I2C keyboard, PWM audio).

set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350 CACHE STRING "Platform")

include(pico_sdk_import.cmake)

project(galagino_picocalc C CXX ASM)
set(CMAKE_C_STANDARD 11)

# At 300 MHz a flash divider of 2 runs flash at 150 MHz, out of spec; 4 gives
# 75 MHz. On RP2350 the divider only takes effect if the XIP setup (boot2) is
# embedded, otherwise flash keeps whatever timing the bootrom left. Both must
# be set before pico_sdk_init() so they reach the boot2 build.
add_compile_definitions(PICO_FLASH_SPI_CLKDIV=4 PICO_EMBED_XIP_SETUP=1)

pico_sdk_init()

set(GALAGINO ${CMAKE_CURRENT_LIST_DIR}/../galagino)
set(COMMON ${CMAKE_CURRENT_LIST_DIR}/../galagino_pico_common)

add_executable(galagino_picocalc
    main.c
    lcd.c
    kbd.c
    audio.c
    ${COMMON}/galagino_core.c
    ${GALAGINO}/Z80.c
    ${GALAGINO}/i8048.c
    ${GALAGINO}/emulation.c
)

pico_generate_pio_header(galagino_picocalc ${CMAKE_CURRENT_LIST_DIR}/lcd.pio)

target_include_directories(galagino_picocalc PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${COMMON}
    ${GALAGINO}
)

# picocalc_config.h defines _CONFIG_H_, which turns the CYD config.h in
# ../galagino into a no-op wherever it is included.
target_compile_options(galagino_picocalc PRIVATE
    -include ${CMAKE_CURRENT_LIST_DIR}/picocalc_config.h
    -O2
)

# emulation.c calls a few Arduino/FreeRTOS functions; map them to the SDK.
set_source_files_properties(${GALAGINO}/emulation.c PROPERTIES
    COMPILE_OPTIONS "-include;${COMMON}/esp32_compat.h"
)

target_link_libraries(galagino_picocalc PRIVATE
    pico_stdlib
    pico_multicore
    pico_sync
    hardware_pio
    hardware_dma
    hardware_pwm
    hardware_i2c
    hardware_vreg
    hardware_clocks
    hardware_watchdog
    pico_rand
)

pico_enable_stdio_usb(galagino_picocalc 1)
pico_enable_stdio_uart(galagino_picocalc 0)

pico_add_extra_outputs(galagino_picocalc)
