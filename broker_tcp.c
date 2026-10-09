#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define MAX_CLIENTES 256      /* conexiones simultaneas */
#define MAX_LINEA 1024        /* tamano maximo de una linea, con su '\n' */
#define MAX_TEMAS 512         /* bytes para la lista de temas de un cliente */
#define MAX_COLA (64 * 1024)  /* bytes pendientes de envio que se toleran por cliente */
#define PUERTO_DEFECTO 9000

struct cliente {
    char entrada[MAX_LINEA]; /* linea a medio recibir */
    size_t n_entrada;
    char temas[MAX_TEMAS];   /* temas suscritos, como " A-B C-D " */
    char cola[MAX_COLA];     /* salida que el nucleo aun no ha aceptado */
    size_t n_cola;
};

/* pfd[0] es el socket de escucha; pfd[i] y cli[i] describen al cliente i. fd = -1: libre. */
static struct pollfd pfd[MAX_CLIENTES + 1];
static struct cliente cli[MAX_CLIENTES + 1];
static int n_pfd = 1;
static volatile sig_atomic_t fin; /* lo activa SIGINT/SIGTERM */

static void al_recibir_senal(int senal)
{
    (void)senal;
    fin = 1;
}

static void morir(const char *donde)
{
    perror(donde);
    exit(EXIT_FAILURE);
}

/* Cierra la conexion i y deja libre su entrada. */
static void cerrar_cliente(int i, const char *motivo)
{
    if (close(pfd[i].fd) < 0)
        perror("close");
    pfd[i].fd = -1;
    pfd[i].events = pfd[i].revents = 0;
    cli[i].n_entrada = cli[i].n_cola = 0;
    cli[i].temas[0] = '\0';
    printf("[-] cliente %d desconectado (%s)\n", i, motivo);
    while (n_pfd > 1 && pfd[n_pfd - 1].fd < 0)
        n_pfd--;
}

static void enviar(int i, const char *datos, size_t len)
{
    struct cliente *c = &cli[i];
    ssize_t r;

    while (c->n_cola == 0 && len > 0) {
        r = send(pfd[i].fd, datos, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (r >= 0) {
            datos += r;
            len -= (size_t)r;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else if (errno != EINTR) {
            perror("send");
            cerrar_cliente(i, "error de envio");
            return;
        }
    }
    if (len == 0)
        return;
    if (c->n_cola + len > MAX_COLA) {
        cerrar_cliente(i, "no lee: cola de salida llena");
        return;
    }
    memcpy(c->cola + c->n_cola, datos, len);
    c->n_cola += len;
    pfd[i].events |= POLLOUT;
}

/* poll() dice que el cliente i vuelve a aceptar datos: se envia lo que quepa de su cola. */
static void vaciar_cola(int i)
{
    struct cliente *c = &cli[i];
    size_t hecho = 0;
    ssize_t r;

    while (hecho < c->n_cola) {
        r = send(pfd[i].fd, c->cola + hecho, c->n_cola - hecho, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (r >= 0) {
            hecho += (size_t)r;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else if (errno != EINTR) {
            perror("send");
            cerrar_cliente(i, "error de envio");
            return;
        }
    }
    c->n_cola -= hecho;
    memmove(c->cola, c->cola + hecho, c->n_cola);
    if (c->n_cola == 0)
        pfd[i].events &= ~POLLOUT;
}

/* Procesa una linea completa del cliente i: len bytes, sin contar el '\n' que sigue. */
static void procesar_linea(int i, char *linea, size_t len)
{
    char clave[MAX_LINEA + 3];
    size_t usado, n_tema;
    int j, destinos = 0;

    /* Los datos de red pueden traer bytes nulos; este protocolo es texto y no los admite. */
    if (len < 3 || linea[1] != ' ' || memchr(linea, '\0', len))
        return;
    linea[len] = '\0';
    n_tema = strcspn(linea + 2, " ");
    if (n_tema == 0)
        return;

    if (linea[0] == 'S') {
        if (linea[2 + n_tema] != '\0') /* un tema no lleva espacios */
            return;
        snprintf(clave, sizeof clave, " %s ", linea + 2);
        if (strstr(cli[i].temas, clave)) /* ya estaba suscrito */
            return;
        usado = strlen(cli[i].temas);
        if (usado == 0) /* la lista empieza por un espacio: " A-B C-D " */
            usado = (size_t)snprintf(cli[i].temas, MAX_TEMAS, " ");
        if (usado + n_tema + 2 > MAX_TEMAS) {
            printf("[!] cliente %d: demasiados temas\n", i);
            return;
        }
        snprintf(cli[i].temas + usado, MAX_TEMAS - usado, "%s ", linea + 2);
        printf("[S] cliente %d se suscribe a %s\n", i, linea + 2);
    } else if (linea[0] == 'P') {
        if (linea[2 + n_tema] != ' ') /* falta "<seq> <texto>" */
            return;
        snprintf(clave, sizeof clave, " %.*s ", (int)n_tema, linea + 2);
        linea[len] = '\n'; /* se reenvia tal cual llego, con su fin de linea */
        for (j = 1; j < n_pfd; j++)
            if (pfd[j].fd >= 0 && strstr(cli[j].temas, clave)) {
                enviar(j, linea + 2, len - 1);
                destinos += pfd[j].fd >= 0;
            }
        printf("[P] %.*s -> %d suscriptores\n", (int)(len - 2), linea + 2, destinos);
    }
}

/* Hay datos (o un cierre) en la conexion i. */
static void leer(int i)
{
    struct cliente *c = &cli[i];
    char *salto;
    size_t resto;
    ssize_t r;

    r = recv(pfd[i].fd, c->entrada + c->n_entrada, MAX_LINEA - c->n_entrada, MSG_DONTWAIT);
    if (r < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        perror("recv");
        cerrar_cliente(i, "error de lectura");
        return;
    }
    if (r == 0) { /* el otro extremo cerro: llego su FIN */
        cerrar_cliente(i, "cerro la conexion");
        return;
    }
    c->n_entrada += (size_t)r;

    /* Puede haber cero, una o varias lineas completas en el buffer. */
    while (pfd[i].fd >= 0 && (salto = memchr(c->entrada, '\n', c->n_entrada))) {
        procesar_linea(i, c->entrada, (size_t)(salto - c->entrada));
        if (pfd[i].fd < 0) /* se cerro a si mismo al reenviar */
            return;
        resto = c->n_entrada - (size_t)(salto + 1 - c->entrada);
        memmove(c->entrada, salto + 1, resto);
        c->n_entrada = resto;
    }
    if (c->n_entrada == MAX_LINEA) { /* linea mas larga que el buffer: se descarta */
        printf("[!] cliente %d: linea demasiado larga, descartada\n", i);
        c->n_entrada = 0;
    }
}

/* Acepta todas las conexiones que esten esperando en el socket de escucha. */
static void aceptar(void)
{
    int fd, i, uno = 1;

    for (;;) {
        fd = accept(pfd[0].fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNABORTED)
                perror("accept");
            return;
        }
        for (i = 1; i < n_pfd && pfd[i].fd >= 0; i++)
            ;
        if (i > MAX_CLIENTES) {
            fprintf(stderr, "[!] tabla llena: conexion rechazada\n");
            if (close(fd) < 0)
                perror("close");
            continue;
        }
        /* Mensajes cortos y en tiempo real: sin Nagle, cada uno sale en su segmento. */
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &uno, sizeof uno) < 0)
            perror("setsockopt TCP_NODELAY");
        /* Sondas del nucleo para detectar a un cliente que desaparecio sin cerrar. */
        if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &uno, sizeof uno) < 0)
            perror("setsockopt SO_KEEPALIVE");
        pfd[i].fd = fd;
        pfd[i].events = POLLIN;
        pfd[i].revents = 0;
        cli[i].n_entrada = cli[i].n_cola = 0;
        cli[i].temas[0] = '\0';
        if (i == n_pfd)
            n_pfd++;
        printf("[+] cliente %d conectado\n", i);
    }
}

int main(int argc, char **argv)
{
    struct sockaddr_in dir;
    struct sigaction sa;
    char *resto;
    long puerto = PUERTO_DEFECTO;
    int escucha, i, uno = 1;

    if (argc > 2) {
        fprintf(stderr, "uso: %s [puerto]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        puerto = strtol(argv[1], &resto, 10);
        if (*resto != '\0' || puerto < 1 || puerto > 65535) {
            fprintf(stderr, "puerto no valido: %s\n", argv[1]);
            return EXIT_FAILURE;
        }
    }

    /* Sin SA_RESTART: la senal interrumpe poll() (EINTR) y el bucle principal termina. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = al_recibir_senal;
    if (sigemptyset(&sa.sa_mask) < 0 || sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        morir("sigaction");

    /* SOCK_NONBLOCK: accept() nunca bloquea aunque la conexion anunciada ya no este. */
    escucha = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (escucha < 0)
        morir("socket");
    /* Permite volver a usar el puerto justo despues de reiniciar el broker (TIME_WAIT). */
    if (setsockopt(escucha, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof uno) < 0)
        morir("setsockopt SO_REUSEADDR");

    memset(&dir, 0, sizeof dir);
    dir.sin_family = AF_INET;
    dir.sin_addr.s_addr = htonl(INADDR_ANY); /* todas las interfaces */
    dir.sin_port = htons((uint16_t)puerto);  /* orden de bytes de red */
    if (bind(escucha, (struct sockaddr *)&dir, sizeof dir) < 0)
        morir("bind");
    if (listen(escucha, 64) < 0)
        morir("listen");

    pfd[0].fd = escucha;
    pfd[0].events = POLLIN;
    setbuf(stdout, NULL); /* el registro se ve al instante */
    printf("[*] broker TCP en el puerto %d (Ctrl-C para salir)\n", ntohs(dir.sin_port));

    while (!fin) {
        if (poll(pfd, (nfds_t)n_pfd, -1) < 0) {
            if (errno == EINTR)
                continue;
            morir("poll");
        }
        if (pfd[0].revents & POLLIN)
            aceptar();
        for (i = 1; i < n_pfd; i++) {
            if (pfd[i].fd >= 0 && (pfd[i].revents & POLLOUT))
                vaciar_cola(i);
            if (pfd[i].fd >= 0 && (pfd[i].revents & (POLLIN | POLLHUP | POLLERR)))
                leer(i);
        }
    }

    /* Cierre limpio: ningun descriptor queda abierto. */
    for (i = n_pfd - 1; i >= 1; i--)
        if (pfd[i].fd >= 0)
            cerrar_cliente(i, "broker detenido");
    if (close(escucha) < 0)
        perror("close");
    printf("[*] broker detenido\n");
    return EXIT_SUCCESS;
}
