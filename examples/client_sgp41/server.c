/*
 * server.c
 * --------
 * Program: Server na IMX6UL, ktory prijima data od dvoch Pico klientov.
 * Autor: Chalykh Dmytrii
 * Verzia: 2
 * --------
 *   Tento program bezi na Linuxe (POSIX sockets) a pocuva TCP porty 8888 a 8889.
 *   Ked sa Pico-klient pripoji, server prijima pakety so senzormi:
 *     - SGP41: voc_raw, nox_raw ("surove" hodnoty)
 *              voc_index, nox_index (indexy, ktore klient uz vypocital)
 *     - SGP30: tvoc, eco2 (hotove hodnoty z SGP30)
 *
 *   Server robi jednoduche spracovanie:
 *     - nazbiera presne 10 paketov (REPORT_N)
 *     - vypise priemerne hodnoty
 *   Preto je pocet n stabilne 10 (ak je siet stabilna a klient posiela pravidelne).
 *
 * Poznamka: Format paketov MUSI presne sediet s klientmi,
 *           inak budu hodnoty posunute alebo nezmyselne.
 */

// Standardne kniznice C pre signaly, typy a vstup/vystup.
#include <signal.h>   // signaly Ctrl+C / ukoncenie procesu
#include <stdbool.h>  // typ bool (true/false)
#include <stdint.h>   // uint16_t/uint32_t
#include <stdio.h>    // printf/fflush
#include <string.h>   // memset
#include <time.h>     // time_t

// Linux / POSIX sockets
// POSIX sokety: adresy, select(), accept(), recv(), close().
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
// Zjednotenie nazvu typu soketa a funkcie zatvorenia.
typedef int socket_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define CLOSESOCK close
static int socket_platform_init(void) { return 0; }
static void socket_platform_cleanup(void) {}

// TCP porty servera (musia sa zhodovat s SERVER_PORT na klientoch).
#define SERVER_PORT_SGP41 8888
#define SERVER_PORT_SGP30 8889

// Kolko paketov nazbierame pred vypisom priemeru.
#define REPORT_N 10

// Format paketu od SGP41 klienta (musi sediet s client_s.c).
// __attribute__((packed)) zaruci, ze nie su pridane ziadne zarovnavacie bajty.
typedef struct __attribute__((packed)) {
    // Pocitadlo / cislo merania
    uint32_t timestamp;

    // "Surove" hodnoty (ticks) zo snimaca SGP41
    uint16_t voc_raw;
    uint16_t nox_raw;

    // Indexy, ktore klient vypocital pomocou Sensirion Gas Index Algorithm
    uint16_t voc_index;
    uint16_t nox_index;
} sensor_data_sgp41_t;

// Format paketu od SGP30 klienta (musi sediet s client_s30.c).
typedef struct __attribute__((packed)) {
    uint32_t timestamp;
    uint16_t tvoc;
    uint16_t eco2;
} sensor_data_sgp30_t;


// running = 1 "bezime"; 0 "zastavujeme".
static volatile sig_atomic_t running = 1;

// server_socket — soket, na ktorom pocuvame pripojenia klientov.
static socket_t server_socket = INVALID_SOCKET;
static socket_t server_socket_sgp30 = INVALID_SOCKET;

static void on_signal(int sig) {
    // Tato funkcia sa zavola pri Ctrl+C alebo pri ukonceni procesu.
    (void)sig;
    running = 0;
    if (server_socket != INVALID_SOCKET) {
        // Zatvorime serverovy soket pre SGP41.
        shutdown(server_socket, SHUT_RDWR);
        CLOSESOCK(server_socket);
        server_socket = INVALID_SOCKET;
    }
    if (server_socket_sgp30 != INVALID_SOCKET) {
        // Zatvorime serverovy soket pre SGP30.
        shutdown(server_socket_sgp30, SHUT_RDWR);
        CLOSESOCK(server_socket_sgp30);
        server_socket_sgp30 = INVALID_SOCKET;
    }
}

// Stav klienta: aktivny soket, buffer pre akumulaciu dat
// a pocet bajtov, ktore uz mame nacitane v buffri.
typedef struct {
    socket_t sock;
    uint8_t buf[64];
    size_t have;
} client_state_t;

static void client_reset(client_state_t* c) {
    // Zatvorime klientsky soket a vycistime buffer.
    if (c->sock != INVALID_SOCKET) {
        CLOSESOCK(c->sock);
    }
    c->sock = INVALID_SOCKET;
    c->have = 0;
}

static void feed_sgp41(client_state_t* c,
                       double* voc_sum,
                       double* nox_sum,
                       double* voci_sum,
                       double* noxi_sum,
                       uint32_t* cnt) {
    // Spracujeme buffer SGP41: pokym je cely paket, vyberieme ho.
    while (c->have >= sizeof(sensor_data_sgp41_t)) {
        sensor_data_sgp41_t d;
        memcpy(&d, c->buf, sizeof(d));
        memmove(c->buf, c->buf + sizeof(d), c->have - sizeof(d));
        c->have -= sizeof(d);

        *voc_sum += d.voc_raw;
        *nox_sum += d.nox_raw;
        *voci_sum += d.voc_index;
        *noxi_sum += d.nox_index;
        (*cnt)++;

         // Ked nazbierame REPORT_N paketov, vypiseme priemerne hodnoty.
         if (*cnt >= REPORT_N) {
            printf("avg10s[sgp41] n=%u voc=%.0f nox=%.0f voci=%.0f noxi=%.0f\n",
                   *cnt,
                   *voc_sum / *cnt,
                   *nox_sum / *cnt,
                   *voci_sum / *cnt,
                   *noxi_sum / *cnt);
            fflush(stdout);

            *voc_sum = 0;
            *nox_sum = 0;
            *voci_sum = 0;
            *noxi_sum = 0;
            *cnt = 0;
        }
    }
}

static void feed_sgp30(client_state_t* c,
                       double* tvoc_sum,
                       double* eco2_sum,
                       uint32_t* cnt) {
    // Spracujeme buffer SGP30: pokym je cely paket, vyberieme ho.
    while (c->have >= sizeof(sensor_data_sgp30_t)) {
        sensor_data_sgp30_t d;
        memcpy(&d, c->buf, sizeof(d));
        memmove(c->buf, c->buf + sizeof(d), c->have - sizeof(d));
        c->have -= sizeof(d);

        *tvoc_sum += d.tvoc;
        *eco2_sum += d.eco2;
        (*cnt)++;

         // Ked nazbierame REPORT_N paketov, vypiseme priemerne hodnoty.
         if (*cnt >= REPORT_N) {
            printf("avg10s[sgp30] n=%u tvoc=%.0f eco2=%.0f\n",
                   *cnt,
                   *tvoc_sum / *cnt,
                   *eco2_sum / *cnt);
            fflush(stdout);

            *tvoc_sum = 0;
            *eco2_sum = 0;
            *cnt = 0;
        }
    }
}

static int setup_listen_socket(uint16_t port, socket_t* out_sock) {
    // Vytvorime TCP soket IPv4.
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;

    int opt = 1;
    // SO_REUSEADDR umoznuje rychly restart servera na rovnakom porte.
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    // Naviazeme soket na port.
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CLOSESOCK(s);
        return -1;
    }

    // Prejdeme do rezimu pocuvania pripojeni klientov.
    if (listen(s, 1) < 0) {
        CLOSESOCK(s);
        return -1;
    }

    *out_sock = s;
    return 0;
}

int main(void) {
    // Priprava soketovej podsustavy.
    if (socket_platform_init() != 0) return 1;

    // Zachytavame signaly, aby sa dal program zastavit Ctrl+C.
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    // Vytvorime dva TCP sokety (IPv4) pre SGP41 a SGP30.
    if (setup_listen_socket(SERVER_PORT_SGP41, &server_socket) != 0) {
        socket_platform_cleanup();
        return 1;
    }
    if (setup_listen_socket(SERVER_PORT_SGP30, &server_socket_sgp30) != 0) {
        CLOSESOCK(server_socket);
        socket_platform_cleanup();
        return 1;
    }

    // Oznamenie, ze server bezi.
    printf("listening sgp41=%d sgp30=%d", SERVER_PORT_SGP41, SERVER_PORT_SGP30);
    fflush(stdout);

    // Stav pre dvoch klientov (SGP41 a SGP30).
    client_state_t sgp41 = {.sock = INVALID_SOCKET, .have = 0};
    client_state_t sgp30 = {.sock = INVALID_SOCKET, .have = 0};

    double voc_sum = 0, nox_sum = 0;
    double voci_sum = 0, noxi_sum = 0;
    uint32_t cnt41 = 0;

    double tvoc_sum = 0, eco2_sum = 0;
    uint32_t cnt30 = 0;

    // Hlavny cyklus servera: cakat na klientov a citat pakety paralelne.
    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_socket, &rfds);
        FD_SET(server_socket_sgp30, &rfds);
        int max_fd = (server_socket > server_socket_sgp30) ? server_socket : server_socket_sgp30;

        if (sgp41.sock != INVALID_SOCKET) {
            FD_SET(sgp41.sock, &rfds);
            if (sgp41.sock > max_fd) max_fd = sgp41.sock;
        }
        if (sgp30.sock != INVALID_SOCKET) {
            FD_SET(sgp30.sock, &rfds);
            if (sgp30.sock > max_fd) max_fd = sgp30.sock;
        }

        // Timeout select: 1 sekunda, aby sme pravidelne "prebudili" cyklus.
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        // select() caka na udalosti na soketoch.
        int r = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (r == 0) {
            // putchar('.');
            fflush(stdout);
            continue;
        }
        if (r < 0) break;

        // Nove pripojenie SGP41.
        if (FD_ISSET(server_socket, &rfds)) {
            socket_t client = accept(server_socket, NULL, NULL);
            if (client != INVALID_SOCKET) {
                client_reset(&sgp41);
                sgp41.sock = client;
                printf("\nclient sgp41 connected\n\n");
                fflush(stdout);
            }
        }

        // Nove pripojenie SGP30.
        if (FD_ISSET(server_socket_sgp30, &rfds)) {
            socket_t client = accept(server_socket_sgp30, NULL, NULL);
            if (client != INVALID_SOCKET) {
                client_reset(&sgp30);
                sgp30.sock = client;
                printf("\nclient sgp30 connected\n\n");
                fflush(stdout);
            }
        }

        // Prijimame data od SGP41.
        if (sgp41.sock != INVALID_SOCKET && FD_ISSET(sgp41.sock, &rfds)) {
            uint8_t tmp[64];
            int n = recv(sgp41.sock, (char*)tmp, (int)sizeof(tmp), 0);
            if (n <= 0) {
                client_reset(&sgp41);
            } else {
                size_t to_copy = (size_t)n;
                if (sgp41.have + to_copy > sizeof(sgp41.buf)) {
                    client_reset(&sgp41);
                } else {
                    memcpy(sgp41.buf + sgp41.have, tmp, to_copy);
                    sgp41.have += to_copy;
                    feed_sgp41(&sgp41, &voc_sum, &nox_sum, &voci_sum, &noxi_sum, &cnt41);
                }
            }
        }

        // Prijimame data od SGP30.
        if (sgp30.sock != INVALID_SOCKET && FD_ISSET(sgp30.sock, &rfds)) {
            uint8_t tmp[64];
            int n = recv(sgp30.sock, (char*)tmp, (int)sizeof(tmp), 0);
            if (n <= 0) {
                client_reset(&sgp30);
            } else {
                size_t to_copy = (size_t)n;
                if (sgp30.have + to_copy > sizeof(sgp30.buf)) {
                    client_reset(&sgp30);
                } else {
                    memcpy(sgp30.buf + sgp30.have, tmp, to_copy);
                    sgp30.have += to_copy;
                    feed_sgp30(&sgp30, &tvoc_sum, &eco2_sum, &cnt30);
                }
            }
        }
    }

    // Ukoncenie: zatvorime serverove sokety.
    if (server_socket != INVALID_SOCKET) CLOSESOCK(server_socket);
    if (server_socket_sgp30 != INVALID_SOCKET) CLOSESOCK(server_socket_sgp30);
    socket_platform_cleanup();
    return 0;
}
