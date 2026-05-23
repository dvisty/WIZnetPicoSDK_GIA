add_library(sgp41_driver
    sgp41/sensirion_common.c
    sgp41/sensirion_i2c.c
    sgp41/sensirion_i2c_hal.c
    sgp41/sgp41_i2c.c
)

target_include_directories(sgp41_driver PUBLIC
    sgp41
)


target_link_libraries(sgp41_driver PUBLIC pico_stdlib)


if(DEFINED PICO_SDK_PATH)
    target_include_directories(sgp41_driver PUBLIC
        ${PICO_SDK_PATH}/src/rp2_common/hardware_i2c/include
        ${PICO_SDK_PATH}/src/common/pico_stdlib_headers/include
        ${PICO_SDK_PATH}/src/rp2_common/hardware_gpio/include
    )
endif()
