#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define MAX_SUSCRIPTORES 256
#define MAX_DATAGRAMA 1472   /* 1500 (MTU Ethernet) - 20 (IP) - 8 (UDP): no se fragmenta */
#define MAX_TEMAS 512        /* bytes para la lista de temas de un suscriptor */
#define BUFFER_RX (1 << 20)  /* buffer de recepcion que se pide al nucleo */
#define PUERTO_DEFECTO 9000

struct suscriptor {
    int en_uso;
    struct sockaddr_in dir; /* a donde hay que enviarle */
    char temas[MAX_TEMAS];  /* temas suscritos, como " A-B C-D " */
};

static struct suscriptor sus[MAX_SUSCRIPTORES];
static int sock;
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

/* Devuelve el indice del suscriptor con esa direccion; si no existe y crear != 0, lo crea. */
static int buscar(const struct sockaddr_in *de, int crear)
{
    int i, libre = -1;

    for (i = 0; i < MAX_SUSCRIPTORES; i++) {
        if (!sus[i].en_uso) {
            if (libre < 0)
                libre = i;
        } else if (sus[i].dir.sin_port == de->sin_port && sus[i].dir.sin_addr.s_addr == de->sin_addr.s_addr) {
            return i;
        }
    }
    if (!crear || libre < 0)
        return -1;
    sus[libre].en_uso = 1;
    sus[libre].dir = *de;
    sus[libre].temas[0] = '\0';
    return libre;
}

/* Procesa una linea de len bytes (sin su '\n') que llego desde la direccion de. */
static void procesar_linea(const struct sockaddr_in *de, char *linea, size_t len)
{
    char clave[MAX_DATAGRAMA + 3], ip[INET_ADDRSTRLEN];
    size_t usado, n_tema;
    int i, destinos = 0;

    /* Los datos de red pueden traer bytes nulos; este protocolo es texto y no los admite. */
    if (len == 0 || memchr(linea, '\0', len))
        return;
    linea[len] = '\0';
    if (!inet_ntop(AF_INET, &de->sin_addr, ip, sizeof ip))
        snprintf(ip, sizeof ip, "?");

    if (len == 1 && linea[0] == 'U') {
        i = buscar(de, 0);
        if (i >= 0) {
            sus[i].en_uso = 0;
            printf("[-] suscriptor %d (%s:%d) se da de baja\n", i, ip, ntohs(de->sin_port));
        }
        return;
    }
    if (len < 3 || linea[1] != ' ')
        return;
    n_tema = strcspn(linea + 2, " ");
    if (n_tema == 0)
        return;

    if (linea[0] == 'S') {
        if (linea[2 + n_tema] != '\0') /* un tema no lleva espacios */
            return;
        i = buscar(de, 1);
        if (i < 0) {
            printf("[!] tabla llena: suscripcion de %s:%d rechazada\n", ip, ntohs(de->sin_port));
            return;
        }
        snprintf(clave, sizeof clave, " %s ", linea + 2);
        if (strstr(sus[i].temas, clave)) /* ya estaba suscrito (un "S" repetido) */
            return;
        usado = strlen(sus[i].temas);
        if (usado == 0) /* la lista empieza por un espacio: " A-B C-D " */
            usado = (size_t)snprintf(sus[i].temas, MAX_TEMAS, " ");
        if (usado + n_tema + 2 > MAX_TEMAS) {
            printf("[!] suscriptor %d: demasiados temas\n", i);
            return;
        }
        snprintf(sus[i].temas + usado, MAX_TEMAS - usado, "%s ", linea + 2);
        printf("[S] suscriptor %d (%s:%d) se suscribe a %s\n", i, ip, ntohs(de->sin_port), linea + 2);
    } else if (linea[0] == 'P') {
        if (linea[2 + n_tema] != ' ') /* falta "<seq> <texto>" */
            return;
        snprintf(clave, sizeof clave, " %.*s ", (int)n_tema, linea + 2);
        linea[len] = '\n'; /* se reenvia tal cual llego, con su fin de linea */
        for (i = 0; i < MAX_SUSCRIPTORES; i++) {
            if (!sus[i].en_uso || !strstr(sus[i].temas, clave))
                continue;
            /* Un datagrama por suscriptor. Que sendto() no falle no significa que llegue. */
            while (sendto(sock, linea + 2, len - 1, 0, (struct sockaddr *)&sus[i].dir, sizeof sus[i].dir) < 0)
                if (errno != EINTR) {
                    perror("sendto");
                    break;
                }
            destinos++;
        }
        printf("[P] %.*s -> %d suscriptores\n", (int)(len - 2), linea + 2, destinos);
    }
}

int main(int argc, char **argv)
{
    static char datos[MAX_DATAGRAMA + 1]; /* +1: sitio para terminar la ultima linea */
    struct sockaddr_in dir, de;
    struct sigaction sa;
    socklen_t tam;
    char *resto, *linea, *salto;
    size_t queda;
    ssize_t r;
    long puerto = PUERTO_DEFECTO;
    int uno = 1, pedido = BUFFER_RX, real = 0;

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

    /* Sin SA_RESTART: la senal interrumpe recvfrom() (EINTR) y el bucle termina. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = al_recibir_senal;
    if (sigemptyset(&sa.sa_mask) < 0 || sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        morir("sigaction");

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        morir("socket");
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof uno) < 0)
        morir("setsockopt SO_REUSEADDR");
    
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &pedido, sizeof pedido) < 0)
        perror("setsockopt SO_RCVBUF");
    tam = sizeof real;
    if (getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &real, &tam) < 0)
        perror("getsockopt SO_RCVBUF");

    memset(&dir, 0, sizeof dir);
    dir.sin_family = AF_INET;
    dir.sin_addr.s_addr = htonl(INADDR_ANY); /* todas las interfaces */
    dir.sin_port = htons((uint16_t)puerto);  /* orden de bytes de red */
    if (bind(sock, (struct sockaddr *)&dir, sizeof dir) < 0)
        morir("bind");

    setbuf(stdout, NULL); /* el registro se ve al instante */
    printf("[*] broker UDP en el puerto %d, buffer de recepcion de %d bytes (Ctrl-C para salir)\n",
           ntohs(dir.sin_port), real);

    while (!fin) {
        /*
         * recvfrom() entrega un datagrama entero y dice quien lo envio. Con MSG_TRUNC
         * devuelve el tamano real aunque no quepa: asi se detecta un datagrama recortado.
         */
        memset(&de, 0, sizeof de);
        tam = sizeof de;
        r = recvfrom(sock, datos, MAX_DATAGRAMA, MSG_TRUNC, (struct sockaddr *)&de, &tam);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            morir("recvfrom");
        }
        if (r > MAX_DATAGRAMA) {
            printf("[!] datagrama de %zd bytes: no cabe, descartado\n", r);
            continue;
        }
        /* Normalmente trae una sola linea; se aceptan varias, y la ultima sin '\n'. */
        for (linea = datos, queda = (size_t)r; queda > 0;) {
            salto = memchr(linea, '\n', queda);
            if (!salto) {
                procesar_linea(&de, linea, queda);
                break;
            }
            procesar_linea(&de, linea, (size_t)(salto - linea));
            queda -= (size_t)(salto + 1 - linea);
            linea = salto + 1;
        }
    }

    if (close(sock) < 0)
        perror("close");
    printf("[*] broker detenido\n");
    return EXIT_SUCCESS;
}
