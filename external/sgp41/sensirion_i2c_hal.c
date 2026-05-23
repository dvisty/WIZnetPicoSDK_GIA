#include "sensirion_i2c_hal.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

#define I2C_PORT i2c0
#define I2C_SDA  4
#define I2C_SCL  5
#define I2C_FREQ 100000

void sensirion_i2c_hal_init(void) {
    i2c_init(I2C_PORT, I2C_FREQ);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
}

void sensirion_i2c_hal_free(void) {
    /* Pico: nothing to free */
}

int8_t sensirion_i2c_hal_read(uint8_t address, uint8_t* data, uint16_t data_len) {
    int ret = i2c_read_blocking(I2C_PORT, address, data, data_len, false);
    return (ret < 0) ? -1 : 0;
}

int8_t sensirion_i2c_hal_write(uint8_t address, const uint8_t* data, uint16_t data_len) {
    int ret = i2c_write_blocking(I2C_PORT, address, data, data_len, false);
    return (ret < 0) ? -1 : 0;
}

void sensirion_i2c_hal_sleep_usec(uint32_t useconds) {
    sleep_us(useconds);
}
