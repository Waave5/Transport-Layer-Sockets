#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#define MAX_CLIENTES 256
#define MAX_LINEA 1024       /* tamano maximo de una linea, con su '\n' */
#define MAX_TEMAS 512        /* bytes para la lista de temas de un cliente */
#define BUFFER_RX (1 << 20)  /* buffer de recepcion que se pide al nucleo para el socket UDP */
#define INACTIVIDAD_MS 30000 /* una conexion sin respuesta durante este tiempo se da por muerta */
#define PUERTO_DEFECTO 9000
#define ALPN "\4neko"        /* un byte de longitud y el nombre del protocolo de aplicacion */
#define CERT "bono-cert.pem"
#define CLAVE "bono-key.pem"

static SSL *con[MAX_CLIENTES];            /* conexion QUIC de cada cliente; NULL = entrada libre */
static SSL *str[MAX_CLIENTES];            /* su stream; NULL hasta que el cliente lo abre */
static char entrada[MAX_CLIENTES][MAX_LINEA]; /* linea a medio recibir: un stream es un flujo de bytes */
static size_t n_entrada[MAX_CLIENTES];
static char temas[MAX_CLIENTES][MAX_TEMAS]; /* temas suscritos, como " A-B C-D " */
static int cerrado[MAX_CLIENTES];         /* 1 si el cliente ya cerro su mitad del stream */
static int desde[MAX_CLIENTES];           /* publicacion a medio repartir: por que suscriptor va */
static int enviados[MAX_CLIENTES];        /* ...y a cuantos se les ha enviado ya */
static int dueno[MAX_CLIENTES];           /* suscriptor con una linea a medio escribir: que cliente (+1) la escribe */
static int n_cli, sock;
static SSL_CONN_CLOSE_INFO info;          /* solo para preguntar si una conexion ya termino */
static volatile sig_atomic_t fin;         /* lo activa SIGINT/SIGTERM */

static void al_recibir_senal(int senal)
{
    (void)senal;
    fin = 1;
}

/* Error de una llamada al sistema. */
static void morir(const char *donde)
{
    perror(donde);
    exit(EXIT_FAILURE);
}

/* Error de la libreria: OpenSSL guarda el motivo en su propia cola de errores. */
static void morir_ssl(const char *que)
{
    fprintf(stderr, "%s\n", que);
    ERR_print_errors_fp(stderr);
    exit(EXIT_FAILURE);
}

/* OpenSSL llama aqui con cada clave de sesion; se guardan en el formato que lee Wireshark. */
static void guardar_clave(const SSL *s, const char *linea)
{
    const char *ruta = getenv("SSLKEYLOGFILE");
    FILE *f;

    (void)s;
    if (!ruta || !(f = fopen(ruta, "a")))
        return;
    fprintf(f, "%s\n", linea);
    if (fclose(f) != 0)
        perror("fclose");
}

/* Durante el handshake: elige el protocolo de aplicacion entre los que ofrece el cliente. Solo vale "neko". */
static int elegir_alpn(SSL *s, const unsigned char **out, unsigned char *n_out, const unsigned char *oferta,
                       unsigned int n_oferta, void *arg)
{
    (void)s;
    (void)arg;
    if (SSL_select_next_proto((unsigned char **)out, n_out, (const unsigned char *)ALPN, 5, oferta, n_oferta) !=
        OPENSSL_NPN_NEGOTIATED)
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    return SSL_TLSEXT_ERR_OK;
}

/* Duerme hasta que llegue un datagrama o hasta que QUIC necesite actuar (retransmitir, etc.). */
static void dormir(SSL *q)
{
    struct pollfd p;
    struct timeval tv;
    int infinito, ms = 1000;

    if (SSL_get_event_timeout(q, &tv, &infinito) && !infinito && tv.tv_sec < 1)
        ms = (int)(tv.tv_usec / 1000) + 1;
    p.fd = sock;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, ms) < 0 && errno != EINTR)
        morir("poll");
}

/* Libera todo lo asociado al cliente i. */
static void soltar(int i, const char *motivo)
{
    int j;

    printf("[-] cliente %d desconectado (%s)\n", i, motivo);
    SSL_free(str[i]); /* acepta NULL */
    SSL_free(con[i]);
    con[i] = str[i] = NULL;
    n_entrada[i] = 0;
    temas[i][0] = '\0';
    cerrado[i] = desde[i] = dueno[i] = 0;
    for (j = 0; j < n_cli; j++)
        if (dueno[j] == i + 1)
            dueno[j] = 0;
}


static int procesar_linea(int i, char *linea, size_t len)
{
    char clave[MAX_LINEA + 3];
    size_t usado, n_tema, escrito;
    int j;

    /* Los datos de red pueden traer bytes nulos; este protocolo es texto y no los admite. */
    if (len < 3 || linea[1] != ' ' || memchr(linea, '\0', len))
        return 1;
    linea[len] = '\0';
    n_tema = strcspn(linea + 2, " ");
    if (n_tema == 0)
        return 1;

    if (linea[0] == 'S') {
        if (linea[2 + n_tema] != '\0') /* un tema no lleva espacios */
            return 1;
        snprintf(clave, sizeof clave, " %s ", linea + 2);
        if (strstr(temas[i], clave)) /* ya estaba suscrito */
            return 1;
        usado = strlen(temas[i]);
        if (usado == 0) /* la lista empieza por un espacio: " A-B C-D " */
            usado = (size_t)snprintf(temas[i], MAX_TEMAS, " ");
        if (usado + n_tema + 2 > MAX_TEMAS) {
            printf("[!] cliente %d: demasiados temas\n", i);
            return 1;
        }
        snprintf(temas[i] + usado, MAX_TEMAS - usado, "%s ", linea + 2);
        printf("[S] cliente %d se suscribe a %s\n", i, linea + 2);
    } else if (linea[0] == 'P') {
        if (linea[2 + n_tema] != ' ') /* falta "<seq> <texto>" */
            return 1;
        snprintf(clave, sizeof clave, " %.*s ", (int)n_tema, linea + 2);
        linea[len] = '\n'; /* se reenvia tal cual llego, con su fin de linea */
        if (desde[i] == 0)
            enviados[i] = 0;
        for (j = desde[i]; j < n_cli; j++) {
            if (!str[j] || !strstr(temas[j], clave))
                continue;
            desde[i] = j;
            if (dueno[j] && dueno[j] != i + 1)
                return 0; /* otro publicador tiene una linea a medias en ese stream */
            if (SSL_write_ex(str[j], linea + 2, len - 1, &escrito)) {
                dueno[j] = 0;
                enviados[i]++;
            } else if (SSL_get_error(str[j], 0) == SSL_ERROR_WANT_WRITE) {
                dueno[j] = i + 1; /* control de flujo: el suscriptor va lento */
                return 0;
            } else {
                soltar(j, "error de envio");
                if (j == i)
                    return 1;
            }
        }
        desde[i] = 0;
        printf("[P] %.*s -> %d suscriptores\n", (int)(len - 2), linea + 2, enviados[i]);
    }
    return 1;
}

/* Atiende al cliente i: despacha sus lineas completas y lee mas. Devuelve 1 si hizo algo. */
static int atender(int i)
{
    size_t leido, resto;
    char *salto;
    int hizo = 0;

    for (;;) {
        while ((salto = memchr(entrada[i], '\n', n_entrada[i]))) {
            if (!procesar_linea(i, entrada[i], (size_t)(salto - entrada[i])))
                return hizo; /* publicacion pendiente: no se lee mas de este cliente */
            if (!con[i]) /* se solto a si mismo al reenviar */
                return 1;
            resto = n_entrada[i] - (size_t)(salto + 1 - entrada[i]);
            memmove(entrada[i], salto + 1, resto);
            n_entrada[i] = resto;
            hizo = 1;
        }
        if (n_entrada[i] == MAX_LINEA) { /* linea mas larga que el buffer: se descarta */
            printf("[!] cliente %d: linea demasiado larga, descartada\n", i);
            n_entrada[i] = 0;
        }
        if (SSL_read_ex(str[i], entrada[i] + n_entrada[i], MAX_LINEA - n_entrada[i], &leido)) {
            n_entrada[i] += leido;
            hizo = 1;
        } else if (SSL_get_error(str[i], 0) == SSL_ERROR_ZERO_RETURN && !cerrado[i]) {
            /* El cliente cerro su mitad del stream y ya se leyo todo: el broker cierra la suya. */
            cerrado[i] = 1;
            temas[i][0] = '\0';
            if (!SSL_stream_conclude(str[i], 0))
                ERR_clear_error();
            return 1;
        } else if (SSL_get_conn_close_info(con[i], &info, sizeof info)) {
            soltar(i, cerrado[i] ? "cerro la conexion" : "conexion terminada");
            return 1;
        } else if (SSL_get_error(str[i], 0) == SSL_ERROR_SSL) {
            soltar(i, "error del stream");
            return 1;
        } else {
            return hizo; /* SSL_ERROR_WANT_READ: no hay nada mas por ahora */
        }
    }
}

int main(int argc, char **argv)
{
    struct sockaddr_in dir;
    struct sigaction sa;
    SSL_CTX *ctx;
    SSL *escucha, *nueva;
    char *resto;
    long puerto = PUERTO_DEFECTO;
    int i, hizo, pedido = BUFFER_RX;

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

    /* Sin SA_RESTART: la senal interrumpe poll() y el bucle principal termina. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = al_recibir_senal;
    if (sigemptyset(&sa.sa_mask) < 0 || sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        morir("sigaction");

    /* QUIC va sobre UDP: el socket es de datagramas. No bloqueante: de esperar se encarga dormir(). */
    sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0)
        morir("socket");
    /* Todas las conexiones comparten este socket; un buffer grande evita perder datagramas en una rafaga. */
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &pedido, sizeof pedido) < 0)
        perror("setsockopt SO_RCVBUF");
    memset(&dir, 0, sizeof dir);
    dir.sin_family = AF_INET;
    dir.sin_addr.s_addr = htonl(INADDR_ANY); /* todas las interfaces */
    dir.sin_port = htons((uint16_t)puerto);  /* orden de bytes de red */
    if (bind(sock, (struct sockaddr *)&dir, sizeof dir) < 0)
        morir("bind");

    /* Contexto de servidor QUIC (solo admite TLS 1.3) con el certificado y la clave del broker. */
    ctx = SSL_CTX_new(OSSL_QUIC_server_method());
    if (!ctx)
        morir_ssl("SSL_CTX_new");
    if (SSL_CTX_use_certificate_chain_file(ctx, CERT) != 1 || SSL_CTX_use_PrivateKey_file(ctx, CLAVE, SSL_FILETYPE_PEM) != 1)
        morir_ssl("falta " CERT " o " CLAVE " (se crean con make)");
    SSL_CTX_set_alpn_select_cb(ctx, elegir_alpn, NULL);
    SSL_CTX_set_keylog_callback(ctx, guardar_clave);

    /* El listener es el equivalente QUIC del socket de escucha de TCP. */
    escucha = SSL_new_listener(ctx, 0);
    if (!escucha || !SSL_set_fd(escucha, sock) || !SSL_listen(escucha))
        morir_ssl("no se pudo crear el listener");
    if (!SSL_set_blocking_mode(escucha, 0))
        morir_ssl("SSL_set_blocking_mode");

    setbuf(stdout, NULL); /* el registro se ve al instante */
    printf("[*] broker QUIC en el puerto %d (Ctrl-C para salir)\n", ntohs(dir.sin_port));

    while (!fin) {
        SSL_handle_events(escucha); /* procesa los datagramas que llegaron y envia lo pendiente */
        hizo = 0;

        /* Conexiones cuyo handshake ya termino. */
        while ((nueva = SSL_accept_connection(escucha, SSL_ACCEPT_CONNECTION_NO_BLOCK))) {
            for (i = 0; i < n_cli && con[i]; i++)
                ;
            if (i == MAX_CLIENTES) {
                fprintf(stderr, "[!] tabla llena: conexion rechazada\n");
                SSL_free(nueva);
                continue;
            }
            if (!SSL_set_blocking_mode(nueva, 0))
                morir_ssl("SSL_set_blocking_mode");
            /* Tiempo sin noticias del otro extremo tras el cual QUIC cierra la conexion. */
            if (!SSL_set_feature_request_uint(nueva, SSL_VALUE_QUIC_IDLE_TIMEOUT, INACTIVIDAD_MS))
                ERR_clear_error();
            con[i] = nueva;
            if (i == n_cli)
                n_cli++;
            hizo = 1;
            printf("[+] cliente %d conectado\n", i);
        }

        for (i = 0; i < n_cli; i++) {
            if (!con[i])
                continue;
            /* El cliente abre el stream con su primer envio; hasta entonces no hay nada que leer. */
            if (!str[i] && !(str[i] = SSL_accept_stream(con[i], SSL_ACCEPT_STREAM_NO_BLOCK))) {
                if (SSL_get_conn_close_info(con[i], &info, sizeof info)) {
                    soltar(i, "conexion terminada");
                    hizo = 1;
                }
                continue;
            }
            hizo |= atender(i);
        }
        ERR_clear_error(); /* los "ahora no" de las llamadas no bloqueantes no se acumulan */
        if (!hizo)
            dormir(escucha);
    }

    /* Cierre limpio: se avisa a cada cliente (CONNECTION_CLOSE) y se libera todo. */
    for (i = 0; i < n_cli; i++)
        if (con[i])
            SSL_shutdown(con[i]);
    SSL_handle_events(escucha);
    for (i = n_cli - 1; i >= 0; i--)
        if (con[i])
            soltar(i, "broker detenido");
    SSL_free(escucha);
    SSL_CTX_free(ctx);
    if (close(sock) < 0)
        perror("close");
    printf("[*] broker detenido\n");
    return EXIT_SUCCESS;
}
