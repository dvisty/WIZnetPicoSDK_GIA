/*
 * Program: TCP klient pre Raspberry Pi Pico W6100 + SGP41
 * Autor: Chalykh Dmytrii
 * Verzia: 3
 * ----------
 *   Toto je program pre Raspberry Pi Pico + Ethernet cip W6100 + snimac SGP41.
 *   Kazdu 1 sekundu:
 *     1) cita "surove" hodnoty VOC/NOx zo SGP41 (voc_raw/nox_raw)
 *     2) prepocita ich na zrozumitelnejsie indexy VOC Index/NOx Index
 *        (robi to Sensirion Gas Index Algorithm)
 *     3) posle tieto data na PC/server cez TCP.
 *   Verzia 3 pouziva dve jadra Pico: jedno jadro cita SGP41 a dava data do fronty,
 *   druhe jadro sa stara o TCP spojenie a odosielanie dat.
 */

#include <stdio.h>      // zakladne funkcie vstupu/vystupu (printf a pod.)
#include <string.h>    // praca s pamatou/retazcami (memset a pod.)
#include <stdint.h>    // typy ako uint16_t/uint32_t

#include "pico/stdlib.h"    // zakladne funkcie Pico (sleep_ms, stdio_init_all)
#include "pico/multicore.h" // spustenie druheho jadra
#include "pico/util/queue.h" // fronta medzi jadrami
#include "hardware/i2c.h"  // hardverovy I2C na Pico
#include "hardware/spi.h"  // hardverovy SPI na Pico
#include "hardware/gpio.h" // ovladanie GPIO (piny)
#include "pico/time.h"     // cas/timery Pico

// Kniznice pre Ethernet cip W6100 (WIZnet ioLibrary)
#include "wizchip_conf.h"  // vseobecna konfiguracia WIZCHIP
#include "w6100.h"         // specificke pre W6100
#include "socket.h"        // TCP/UDP sokety (connect/send/close a pod.)

// Kniznice pre Sensirion SGP41 (citanie hodnot cez I2C)
#include "sgp41_i2c.h"          // funkcie sgp41_measure_raw_signals(), conditioning...
#include "sensirion_common.h"   // pomocne typy/utility Sensirion
#include "sensirion_i2c_hal.h"  // HAL rozhranie pre I2C (realizujeme nizsie)

// Sensirion Gas Index Algorithm: prevod zo "suroveho" SRAW -> index (VOC/NOx Index)
#include "sensirion_gas_index_algorithm.h"

// Parametre kompenzacie pre SGP41.
// Pouzivame "typicke" RH/T, lebo teplotu/vlhkost nemerieme samostatnym snimacom.
// Hodnoty v "ticks" (format, ktory ocakava SGP41).
#define DEFAULT_RH 0x8000
#define DEFAULT_TEMP 0x6666

// Nastavenie SPI pre W6100:
// SPI_PORT — ktory SPI kontroler Pico pouzivame.
// PIN_* — ktore presne piny su pripojene k W6100.
#define SPI_PORT spi0
#define PIN_SCK  18   // hodinovy signal SPI (SCK)
#define PIN_MOSI 19   // data z Pico do W6100 (MOSI)
#define PIN_MISO 16   // data z W6100 do Pico (MISO)
#define PIN_CS   17   // chip select (vyber cipu)
#define PIN_RST  20   // reset (reset W6100)

// Nastavenie siete pre TCP klienta:
// ETHERNET_SOCKET — cislo soketa na W6100, ktory pouzivame.
// SERVER_IP/PORT — kam sa pripajame (PC/server).
#define ETHERNET_SOCKET 0
#define SERVER_IP {192, 168, 1, 100}
#define SERVER_PORT 8888

// Sietova konfiguracia W6100 (staticka IP adresa).
wiz_NetInfo net_info = {
    .mac = {0x00, 0x08, 0xDC, 0x12, 0x34, 0x56},
    .ip = {192, 168, 1, 50},
    .sn = {255, 255, 255, 0},
    .gw = {192, 168, 1, 1},
    .dns = {8, 8, 8, 8},
    .dhcp = NETINFO_STATIC
};

// Struktura dat pre prenos cez TCP.
// Je "packed" — bez vyrovnavacich bajtov, aby velkost bola presna.
// Rovnaky format musi poznat aj server (server.c).
typedef struct __attribute__((packed)) {
    uint32_t timestamp;   // pocitadlo merani (1,2,3,...) — pre prehladnost
    uint16_t voc_raw;     // "surova" hodnota VOC (SRAW_VOC) zo SGP41
    uint16_t nox_raw;     // "surova" hodnota NOx (SRAW_NOX) zo SGP41
    uint16_t voc_index;   // VOC Index (prepocitany algoritmom)
    uint16_t nox_index;   // NOx Index (prepocitany algoritmom)
} sensor_data_t;

// Fronta na prenos merani medzi jadrami.
static queue_t sensor_queue;


// ============================================
// HAL implementacia pre I2C Sensirion driver
// ============================================

void sensirion_i2c_hal_init(void) {
    // Inicializujeme I2C0 na frekvencii 100 kHz.
    i2c_init(i2c0, 100000);
    // Nastavime piny 4(SDA) a 5(SCL) do rezimu I2C.
    gpio_set_function(4, GPIO_FUNC_I2C);
    gpio_set_function(5, GPIO_FUNC_I2C);
    // Zapneme pull-up rezistory pre I2C linky.
    gpio_pull_up(4);
    gpio_pull_up(5);
}



void sensirion_i2c_hal_free(void) {
    // Vypneme I2C0 (ak by bolo treba ukoncit pracu).
    i2c_deinit(i2c0);
}

int8_t sensirion_i2c_hal_read(uint8_t addr, uint8_t* data, uint16_t len) {
    // Precitame len bajtov z I2C zariadenia na adrese addr.
    // Vratime 0 ak je vsetko OK, alebo -1 pri chybe.
    return (i2c_read_blocking(i2c0, addr, data, len, false) == len) ? 0 : -1;
}

int8_t sensirion_i2c_hal_write(uint8_t addr, const uint8_t* data, uint16_t len) {
    // Zapiseme len bajtov do I2C zariadenia na adrese addr.
    // Vratime 0 ak je vsetko OK, alebo -1 pri chybe.
    return (i2c_write_blocking(i2c0, addr, data, len, false) == len) ? 0 : -1;
}

void sensirion_i2c_hal_sleep_usec(uint32_t usec) {
    // Pauza v mikrosekundach (potrebne pre driver Sensirion).
    sleep_us(usec);
}



// ============================================
// W6100 SPI funkcie
// ============================================

void w6100_select(void) {
    // CS=0 znamena "vybrat" W6100 na SPI zbernici.
    gpio_put(PIN_CS, 0);
}

void w6100_deselect(void) {
    // CS=1 znamena "uvolnit" W6100 na SPI zbernici.
    gpio_put(PIN_CS, 1);
}

// ---- 1. Byte-level callbacks (OK) ----

uint8_t w6100_spi_readbyte(void) {
    // Precitame 1 bajt cez SPI (W6100 ocakava tieto callbacks).
    uint8_t data;
    spi_read_blocking(SPI_PORT, 0xFF, &data, 1);
    return data;
}

void w6100_spi_writebyte(uint8_t data) {
    // Zapiseme 1 bajt cez SPI.
    spi_write_blocking(SPI_PORT, &data, 1);
}

// ---- 2. Burst API pre reg_wizchip_spi_cbfunc (pouziva datasize_t) ----

void w6100_spi_readburst_cb(uint8_t* buf, datasize_t len) {
    // Precitame viac bajtov (burst) — verzia API s datasize_t.
    spi_read_blocking(SPI_PORT, 0xFF, buf, (size_t)len);
}

void w6100_spi_writeburst_cb(uint8_t* buf, datasize_t len) {
    // Zapiseme viac bajtov (burst) — verzia API s datasize_t.
    spi_write_blocking(SPI_PORT, buf, (size_t)len);
}

// ---- 3. Burst API pre reg_wizchip_spiburst_cbfunc (pouziva uint16_t) ----

void w6100_spi_readburst16(uint8_t* buf, uint16_t len) {
    // Precitame viac bajtov (burst) — verzia API s uint16_t.
    spi_read_blocking(SPI_PORT, 0xFF, buf, len);
}

void w6100_spi_writeburst16(uint8_t* buf, uint16_t len) {
    // Zapiseme viac bajtov (burst) — verzia API s uint16_t.
    spi_write_blocking(SPI_PORT, buf, len);
}


// ============================================
// Inicializacia W6100
// ============================================

int w6100_init(void) {
    // 1) Nastavime SPI na 5 MHz.
    spi_init(SPI_PORT, 5000000);
    // 2) Prepneme potrebne piny do rezimu SPI.
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);

    // 3) Pin CS: vystup, predvolene = 1 (nevybrane).
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    // 4) Hardverovy reset W6100: najprv 0, potom 1.
    gpio_init(PIN_RST);
    gpio_set_dir(PIN_RST, GPIO_OUT);
    gpio_put(PIN_RST, 0);
    sleep_ms(20);
    gpio_put(PIN_RST, 1);
    sleep_ms(100);

    // 5) Zaregistrujeme callbacks (funkcie), aby kniznica WIZnet vedela
    //    ako ovladat CS a ako citat/zapisovat cez SPI.
    reg_wizchip_cs_cbfunc(w6100_select, w6100_deselect);

    // Pre normal API (datasize_t) - s explicitnymi castami pre W6100
    reg_wizchip_spi_cbfunc(
        (uint8_t (*)(void))w6100_spi_readbyte,
        (void (*)(uint8_t))w6100_spi_writebyte,
        (void (*)(uint8_t*, datasize_t))w6100_spi_readburst_cb,
        (void (*)(uint8_t*, datasize_t))w6100_spi_writeburst_cb
    );

    // Pre burst API (uint16_t)
    reg_wizchip_spiburst_cbfunc(
        w6100_spi_readburst16,
        w6100_spi_writeburst16
    );

    // 6) Inicializujeme pamat bufferov W6100 pre sokety.
    //    memsize: [RX][socket] a [TX][socket] v kilobajtoch.
    //    Pouzivame iba socket 0 => dame mu 8KB RX a 8KB TX.
    uint8_t memsize[2][8] = {
        {8, 0, 0, 0, 0, 0, 0, 0},
        {8, 0, 0, 0, 0, 0, 0, 0}
    };

    // Ak inicializacia zlyha, vratime -1.
    if (ctlwizchip(CW_INIT_WIZCHIP, memsize) != 0) {
        return -1;
    }

    // 7) Nastavime sietove parametre (IP, maska, brana...).
    NETUNLOCK();
    if (ctlnetwork(CN_SET_NETINFO, (void*)&net_info) != 0) {
        return -1;
    }

    return 0;
}


// ============================================
// TCP funkcie
// ============================================

int tcp_connect_server(void) {
    // Funkcia: pripojit sa k serveru cez TCP.
    // Vrati 0 ak spojenie existuje, alebo -1 ak to zlyha.
    uint8_t server_ip[4] = SERVER_IP;
    uint8_t status;

    status = getSn_SR(ETHERNET_SOCKET);
    if(status != SOCK_CLOSED) {
        // Ak soket nie je zatvoreny, zavrieme ho pre cisty start.
        close(ETHERNET_SOCKET);
    }

    if(socket(ETHERNET_SOCKET, Sn_MR_TCP, 0, 0) < 0) {
        // Vytvorenie TCP soketa zlyhalo.
        return -1;
    }

    if(connect(ETHERNET_SOCKET, server_ip, SERVER_PORT) != SOCK_OK) {
        // Nepodarilo sa pripojit — zatvorime soket.
        close(ETHERNET_SOCKET);
        return -1;
    }

    // Cakame, kym sa spojenie stane "ESTABLISHED".
    // timeout=50 * 100ms = priblizne 5 sekund.
    int timeout = 50;
    while(timeout > 0) {
        status = getSn_SR(ETHERNET_SOCKET);
        if(status == SOCK_ESTABLISHED) {
            // Spojenie je nadviazane.
            return 0;
        }
        if(status == SOCK_CLOSED) {
            // Spojenie sa prerusilo.
            return -1;
        }
        sleep_ms(100);
        timeout--;
    }
    // Ak sme sa nepripojili v case timeoutu, zatvorime soket.
    close(ETHERNET_SOCKET);
    return -1;
}

int tcp_send_data(sensor_data_t *data) {
    // Funkcia: odoslat JEDEN cely paket sensor_data_t cez TCP.
    // Dolezite: send() moze poslat nie vsetky bajty naraz,
    // preto opakujeme cyklus, kym neposleme cely paket.
    const uint8_t* p = (const uint8_t*)data;
    uint16_t remaining = (uint16_t)sizeof(sensor_data_t);

    while (remaining > 0) {
        // Posielame cast/cele zvysne data.
        int32_t sent = send(ETHERNET_SOCKET, (uint8_t*)p, remaining);
        if (sent > 0) {
            // Ak sa nieco odoslalo, posunieme ukazovatel a znizime zvysok.
            p += (uint16_t)sent;
            remaining -= (uint16_t)sent;
            continue;
        }
        if (sent == 0) {
            // 0 bajtov — niekedy docasna situacia, trochu pockame.
            sleep_ms(1);
            continue;
        }
        // sent < 0 => chyba
        return -1;
    }

    // Ak sme sa dostali sem, vsetko sa odoslalo uspesne.
    return 0;
}

static void core1_main(void) {
    // Inicializacia Ethernet cipu W6100 na druhom jadre.
    if (w6100_init() != 0) {
        // Ak Ethernet neprejde inicializaciou, zastavime sa.
        while (true) {
            sleep_ms(1000);
        }
    }
    // Pre istotu zavrieme soket 0 pred startom.
    close(ETHERNET_SOCKET);

    // Hlavny cyklus: berieme data z fronty a posielame na server.
    while (true) {
        sensor_data_t data;
        queue_remove_blocking(&sensor_queue, &data);

        // Kontrolujeme, ci TCP spojenie este zije. Ak nie, pripojime sa znova.
        if (getSn_SR(ETHERNET_SOCKET) != SOCK_ESTABLISHED) {
            close(ETHERNET_SOCKET);
            tcp_connect_server();
        }

        // Posleme paket cez TCP.
        if (tcp_send_data(&data) != 0) {
            // Ak odoslanie zlyha, zavrieme soket, aby sme sa nabuduce znovu pripojili.
            close(ETHERNET_SOCKET);
        }
    }
}

// ============================================
// Hlavny program
// ============================================

int main() {
    // Start programu.
    stdio_init_all();

    // Kratka pauza po starte, aby sa vsetko stihlo stabilizovat.
    sleep_ms(3000);

    // Inicializujeme frontu a spustime druhe jadro pre TCP.
    queue_init(&sensor_queue, sizeof(sensor_data_t), 8);
    multicore_launch_core1(core1_main);

    // Inicializacia I2C pre SGP41.
    sensirion_i2c_hal_init();

    // Kondicionovanie SGP41 (priblizne 10 sekund).
    // Je to "zohriatie"/priprava, aby boli hodnoty stabilnejsie.
    uint16_t voc_raw;
    for (int i = 0; i < 10; i++) {
        if (sgp41_execute_conditioning(DEFAULT_RH, DEFAULT_TEMP, &voc_raw) != 0) {
            // Ak kondicionovanie zlyha, koncime.
            return -1;
        }
        sleep_ms(1000);
    }

    // Pocitadlo merani (bude rast 1,2,3,...)
    uint32_t measurement_count = 0;

    // Inicializacia algoritmu Gas Index.
    // Dolezite: algoritmus ma vnutorny stav, ktory sa aktualizuje kazdu sekundu.
    GasIndexAlgorithmParams voc_params;
    GasIndexAlgorithm_init(&voc_params, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);
    GasIndexAlgorithmParams nox_params;
    GasIndexAlgorithm_init(&nox_params, GasIndexAlgorithm_ALGORITHM_TYPE_NOX);

    // Hlavny nekonecny cyklus merani.
    while(true) {
        // Tu budeme ukladat "surove" hodnoty zo snimaca.
        uint16_t voc_raw, nox_raw;

        // 1) Citame SRAW_VOC a SRAW_NOX zo SGP41.
        if (sgp41_measure_raw_signals(DEFAULT_RH, DEFAULT_TEMP, &voc_raw, &nox_raw) == 0) {
            measurement_count++;

            // 2) Vypocitame indexy (VOC Index a NOx Index) zo "surovych" hodnot.
            //    Prvych ~45 sekund moze algoritmus vracat 0/default (je to normalne).
            int32_t voc_index_value = 0;
            int32_t nox_index_value = 0;
            GasIndexAlgorithm_process(&voc_params, (int32_t)voc_raw, &voc_index_value);
            GasIndexAlgorithm_process(&nox_params, (int32_t)nox_raw, &nox_index_value);

            // 3) Vytvorime paket na odoslanie.
            sensor_data_t data = {
                .timestamp = measurement_count,
                .voc_raw = voc_raw,
                .nox_raw = nox_raw,
                // Prevod int32 -> uint16 (zapornym hodnotam dame 0).
                .voc_index = (uint16_t)((voc_index_value < 0) ? 0 : voc_index_value),
                .nox_index = (uint16_t)((nox_index_value < 0) ? 0 : nox_index_value),
            };

            // 4) Dáme data do fronty pre odoslanie druhym jadrom.
            queue_add_blocking(&sensor_queue, &data);
        }

        // 6) Pockame 1 sekundu, aby sme mali stabilny interval 1 Hz.
        sleep_ms(1000);
    }

    return 0;
}