/*
 * Program: TCP klient pre Raspberry Pi Pico W6100 + SGP30
 * Autor: Chalykh Dmytrii
 * Verzia: 1
 * ----------
 *   Tento program bezi na Raspberry Pi Pico + Ethernet cip W6100 + snimac SGP30.
 *   Kazdu 1 sekundu:
 *     1) spusti meranie TVOC a eCO2 na SGP30
 *     2) skontroluje CRC prijatych hodnot
 *     3) posle vysledky na PC/server cez TCP (port 8889)
 */

// Zakladne kniznice C.
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// Pico SDK a hardverove periferie.
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

// WIZnet (Ethernet cip W6100)
#include "wizchip_conf.h"
#include "w6100.h"
#include "socket.h"

// SGP30 (I2C HAL od Sensirion)
#include "sensirion_i2c_hal.h"

//  KONFIGURACIA 

// I2C nastavenie pre SGP30.
#define I2C_PORT i2c0
#define I2C_SDA 4
#define I2C_SCL 5

// SPI nastavenie pre W6100.
#define SPI_PORT spi0
#define PIN_SCK  18
#define PIN_MOSI 19
#define PIN_MISO 16
#define PIN_CS   17
#define PIN_RST  20

// TCP klient (socket, IP a port servera).
#define SOCKET_NUM 0
#define SERVER_PORT 8889
#define SERVER_IP {192,168,1,100}

// I2C adresa SGP30.
#define SGP30_I2C_ADDR 0x58

//  DATA 

typedef struct __attribute__((packed)) {
    uint32_t timestamp;
    uint16_t tvoc;
    uint16_t eco2;
} sensor_data_t;

// Vypocet CRC8 podla datasheetu SGP30.
// CRC sa pocita nad 2 bajtmi a porovnava s tretim bajtom.
static uint8_t sgp30_crc8(const uint8_t* data, uint8_t len) {
    uint8_t crc = 0xFF;

    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ 0x31);
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

// Posle 16-bitovy prikaz do SGP30 cez I2C.
// SGP30 pouziva big-endian poradie bajtov prikazu.
static int sgp30_write_command(uint16_t cmd) {
    uint8_t tx[2] = {(uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF)};
    return sensirion_i2c_hal_write(SGP30_I2C_ADDR, tx, sizeof(tx));
}

// Inicializacia IAQ merania v SGP30.
// Bez tohto kroku senzor nevracia platne hodnoty TVOC/eCO2.
int16_t sgp30_iaq_init(void) {
    if (sgp30_write_command(0x2003) != 0) {
        return -1;
    }
    sleep_ms(10);
    return 0;
}

// Zmeria TVOC a eCO2 (blokujuca verzia, caka na data).
// Vracia 0 pri uspechu, -1 pri chybe I2C alebo CRC.
int16_t sgp30_measure_iaq_blocking_read(uint16_t* tvoc, uint16_t* eco2) {
    uint8_t rx[6];

    // Spustime meranie IAQ (TVOC + eCO2).
    if (sgp30_write_command(0x2008) != 0) {
        return -1;
    }

    // Minimalny cas merania podla datasheetu.
    sleep_ms(12);

    // Precitame 2x16-bit hodnoty + CRC (6 bajtov).
    // Format: TVOC_MSB, TVOC_LSB, CRC, ECO2_MSB, ECO2_LSB, CRC
    if (sensirion_i2c_hal_read(SGP30_I2C_ADDR, rx, sizeof(rx)) != 0) {
        return -1;
    }

    // Overime CRC pre obe 16-bit hodnoty.
    if (sgp30_crc8(&rx[0], 2) != rx[2] || sgp30_crc8(&rx[3], 2) != rx[5]) {
        return -1;
    }

    // Rozbalime big-endian 16-bit hodnoty.
    *eco2 = (uint16_t)(((uint16_t)rx[0] << 8) | rx[1]);
    *tvoc = (uint16_t)(((uint16_t)rx[3] << 8) | rx[4]);
    return 0;
}

//  I2C HAL 

void sensirion_i2c_hal_init(void) {
    // Inicializacia I2C0 na 100 kHz.
    i2c_init(I2C_PORT, 100000);
    // Nastavenie pinov pre I2C.
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    // Zapnutie pull-up rezistorov (I2C vyzaduje pull-up na SDA/SCL).
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
}

int8_t sensirion_i2c_hal_read(uint8_t addr, uint8_t* data, uint16_t len) {
    // Citanie len bajtov z I2C zariadenia.
    // Vrati 0 pri uspechu, -1 pri chybe.
    return (i2c_read_blocking(I2C_PORT, addr, data, len, false) == len) ? 0 : -1;
}

int8_t sensirion_i2c_hal_write(uint8_t addr, const uint8_t* data, uint16_t len) {
    // Zapis len bajtov do I2C zariadenia.
    // Vrati 0 pri uspechu, -1 pri chybe.
    return (i2c_write_blocking(I2C_PORT, addr, data, len, false) == len) ? 0 : -1;
}

void sensirion_i2c_hal_sleep_usec(uint32_t usec) {
    // Pauza v mikrosekundach.
    sleep_us(usec);
}

//  W6100 SPI 

// Vyber/odvyber W6100 na SPI zbernici.
// WIZnet kniznica vola tieto funkcie pri kazdej SPI transakcii.
void wiz_select() { gpio_put(PIN_CS, 0); }
void wiz_deselect() { gpio_put(PIN_CS, 1); }

uint8_t wiz_readbyte(void) {
    // Citanie jedneho bajtu cez SPI.
    uint8_t d;
    spi_read_blocking(SPI_PORT, 0xFF, &d, 1);
    return d;
}

void wiz_writebyte(uint8_t d) {
    // Zapis jedneho bajtu cez SPI.
    spi_write_blocking(SPI_PORT, &d, 1);
}

void wiz_readburst(uint8_t* buf, datasize_t len) {
    // Burst citanie cez SPI (datasize_t).
    spi_read_blocking(SPI_PORT, 0xFF, buf, (size_t)len);
}

void wiz_writeburst(uint8_t* buf, datasize_t len) {
    // Burst zapis cez SPI (datasize_t).
    spi_write_blocking(SPI_PORT, buf, (size_t)len);
}

void wiz_readburst16(uint8_t* buf, uint16_t len) {
    // Burst citanie cez SPI (uint16_t).
    spi_read_blocking(SPI_PORT, 0xFF, buf, len);
}

void wiz_writeburst16(uint8_t* buf, uint16_t len) {
    // Burst zapis cez SPI (uint16_t).
    spi_write_blocking(SPI_PORT, buf, len);
}

//  NETWORK 

wiz_NetInfo netinfo = {
    .mac = {0x00,0x08,0xDC,0x11,0x22,0x33},
    .ip  = {192,168,1,51},
    .sn  = {255,255,255,0},
    .gw  = {192,168,1,1},
    .dns = {8,8,8,8},
    .dhcp = NETINFO_STATIC
};

int w6100_init_all() {
    // Inicializacia SPI pre W6100.
    spi_init(SPI_PORT, 5000000);

    // Nastavenie SPI pinov.
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);

    // CS pin ako vystup.
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    // Reset pin pre W6100.
    gpio_init(PIN_RST);
    gpio_set_dir(PIN_RST, GPIO_OUT);

    // Hardverovy reset W6100 (kratky pulz 0->1).
    gpio_put(PIN_RST, 0);
    sleep_ms(50);
    gpio_put(PIN_RST, 1);
    sleep_ms(100);

    // Registracia callbackov pre WIZnet kniznicu.
    reg_wizchip_cs_cbfunc(wiz_select, wiz_deselect);
    reg_wizchip_spi_cbfunc(
        (uint8_t (*)(void))wiz_readbyte,
        (void (*)(uint8_t))wiz_writebyte,
        (void (*)(uint8_t*, datasize_t))wiz_readburst,
        (void (*)(uint8_t*, datasize_t))wiz_writeburst
    );
    reg_wizchip_spiburst_cbfunc(wiz_readburst16, wiz_writeburst16);

    // Konfiguracia RX/TX bufferov pre sokety.
    // Pouzivame len socket 0 => 8 KB RX/TX.
    uint8_t mem[2][8] = {
        {8,0,0,0,0,0,0,0},
        {8,0,0,0,0,0,0,0}
    };

    // Inicializacia W6100.
    if (ctlwizchip(CW_INIT_WIZCHIP, mem) != 0) return -1;
    // Nastavenie sietovych parametrov.
    NETUNLOCK();
    if (ctlnetwork(CN_SET_NETINFO, &netinfo) != 0) return -1;

    return 0;
}

//  TCP 

int tcp_connect() {
    // Pripojenie k serveru cez TCP.
    // Caka sa na stav SOCK_ESTABLISHED.
    uint8_t ip[4] = SERVER_IP;
    uint8_t status;

    status = getSn_SR(SOCKET_NUM);
    if (status != SOCK_CLOSED) {
        // Ak soket nie je zatvoreny, zavrieme ho.
        close(SOCKET_NUM);
    }

    // Vytvorime TCP soket.
    if (socket(SOCKET_NUM, Sn_MR_TCP, 0, 0) < 0) return -1;
    if (connect(SOCKET_NUM, ip, SERVER_PORT) != SOCK_OK) {
        // Ak sa nepodarilo pripojit, zavrieme soket.
        close(SOCKET_NUM);
        return -1;
    }

    // Cakame na stav ESTABLISHED (timeout ~5 s).
    int timeout = 50;
    while (timeout--) {
        status = getSn_SR(SOCKET_NUM);
        if (status == SOCK_ESTABLISHED) return 0;
        if (status == SOCK_CLOSED) return -1;
        sleep_ms(100);
    }

    // Ak timeout vyprsal, zavrieme soket.
    close(SOCKET_NUM);
    return -1;
}

int tcp_send(sensor_data_t* data) {
    // Odoslanie celeho paketu cez TCP (send moze poslat len cast).
    // Funkcia sa stara o opakovane odoslanie, kym neposle vsetko.
    uint8_t* p = (uint8_t*)data;
    uint16_t len = sizeof(sensor_data_t);

    while (len > 0) {
        int32_t s = send(SOCKET_NUM, p, len);
        if (s > 0) {
            p += s;
            len -= s;
            continue;
        }
        if (s == 0) {
            // Docasne nic neposlane, kratko pockame.
            sleep_ms(1);
            continue;
        }
        return -1;
    }
    return 0;
}

//  MAIN 

int main() {
    // Start programu.
    stdio_init_all();
    sleep_ms(2000);

    // Inicializacia I2C pre SGP30.
    sensirion_i2c_hal_init();

    // Inicializacia W6100 a siete.
    if (w6100_init_all() != 0) return -1;
    close(SOCKET_NUM);

    // Inicializacia SGP30.
    if (sgp30_iaq_init() != 0) {
        printf("SGP30 init error\n");
        return -1;
    }

    // Informacny vypis.
    printf("System started\n");

    // Pocitadlo merani (timestamp).
    uint32_t counter = 0;

    // Hlavny meraci cyklus.
    while (1) {
        uint16_t tvoc, eco2;

        if (sgp30_measure_iaq_blocking_read(&tvoc, &eco2) == 0) {

            counter++;

            // Vypis na serial konzolu.
            printf("TVOC=%d ppb, eCO2=%d ppm\n", tvoc, eco2);

            // Ak nie je TCP spojenie, pripojime sa.
            if (getSn_SR(SOCKET_NUM) != SOCK_ESTABLISHED) {
                tcp_connect();
            }

            // Naplnime paket pre odoslanie.
            sensor_data_t data = {
                .timestamp = counter,
                .tvoc = tvoc,
                .eco2 = eco2
            };

            // Posleme paket, pri chybe zavrieme soket.
            if (tcp_send(&data) != 0) {
                close(SOCKET_NUM);
            }
        }

        // 1 Hz interval merania.
        sleep_ms(1000);
    }
}