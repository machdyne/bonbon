cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(bonbon)

add_executable(bonbon
        bonbon.c
        )

target_sources(bonbon PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/bonbon.c
        ${CMAKE_CURRENT_LIST_DIR}/pio_spi.c
        )

pico_sdk_init()
pico_generate_pio_header(bonbon ${CMAKE_CURRENT_LIST_DIR}/spi.pio)

#pico_enable_stdio_usb(bonbon 1)
#pico_enable_stdio_uart(bonbon 0)

target_link_libraries(bonbon PRIVATE pico_stdlib hardware_resets hardware_irq hardware_spi hardware_pio)

pico_add_extra_outputs(bonbon)
