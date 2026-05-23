set(TARGET_NAME client_sgp30)

add_executable(${TARGET_NAME}
        ${TARGET_NAME}.c
        )

target_link_libraries(${TARGET_NAME} PRIVATE
        pico_stdlib
        hardware_i2c
        hardware_spi
        hardware_gpio
        IOLIBRARY_FILES
        ETHERNET_FILES
        sgp41_driver
        )

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})