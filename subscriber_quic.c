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
#include <sys/wait.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#define MAX_LINEA 1024 /* tamano maximo de una linea, con su '\n' (el mismo del broker) */
#define MAX_TEMA 64
#define MAX_TEMAS 16   /* temas por suscriptor */
#define MAX_SUSCRIPTORES 32
#define INACTIVIDAD_MS 30000 /* sin respuesta del broker durante este tiempo, la conexion se da por muerta */
#define ALPN "\4neko"        /* un byte de longitud y el nombre del protocolo de aplicacion */
#define CERT "bono-cert.pem"

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

/* Pide un texto por la terminal; Enter sin escribir nada deja el valor por defecto. */
static void preguntar(const char *texto, const char *defecto, char *out, size_t cap)
{
    printf("%s [%s]: ", texto, defecto);
    fflush(stdout);
    if (!fgets(out, (int)cap, stdin)) {
        fprintf(stderr, "\nentrada cerrada\n");
        exit(EXIT_FAILURE);
    }
    out[strcspn(out, "\r\n")] = '\0';
    if (out[0] == '\0')
        snprintf(out, cap, "%s", defecto);
}

/* Convierte un texto en un entero dentro de [min, max]; devuelve 0 si no lo es. */
static int leer_entero(const char *texto, long min, long max, long *valor)
{
    char *resto;

    errno = 0;
    *valor = strtol(texto, &resto, 10);
    return errno == 0 && resto != texto && *resto == '\0' && *valor >= min && *valor <= max;
}

/* Pide un numero dentro de [min, max] hasta que sea valido. */
static long preguntar_numero(const char *texto, long defecto, long min, long max)
{
    char linea[64], def[32];
    long valor;

    snprintf(def, sizeof def, "%ld", defecto);
    for (;;) {
        preguntar(texto, def, linea, sizeof linea);
        if (leer_entero(linea, min, max, &valor))
            return valor;
        printf("  escribe un numero entre %ld y %ld\n", min, max);
    }
}

static int partir_temas(char *texto, char **temas)
{
    char *estado, *t;
    int n = 0;

    for (t = strtok_r(texto, " \t", &estado); t; t = strtok_r(NULL, " \t", &estado)) {
        if (n == MAX_TEMAS || strlen(t) >= MAX_TEMA)
            return 0;
        temas[n++] = t;
    }
    return n;
}

/* Escribe len bytes completos en fd (write() tambien puede ser parcial). Devuelve 0 o -1. */
static int escribir_todo(int fd, const char *datos, size_t len)
{
    ssize_t r;

    while (len > 0) {
        r = write(fd, datos, len);
        if (r < 0) {
            if (errno == EINTR && !fin)
                continue;
            return -1;
        }
        datos += r;
        len -= (size_t)r;
    }
    return 0;
}

/* Error de la libreria: OpenSSL guarda el motivo en su propia cola de errores. */
static void error_ssl(const char *prefijo, const char *que)
{
    fprintf(stderr, "%s%s\n", prefijo, que);
    ERR_print_errors_fp(stderr);
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

static SSL *conectar(const char *prefijo, const char *ip, long puerto, SSL_CTX **ctx, int *sock)
{
    struct sockaddr_in dir;
    BIO_ADDR *destino = NULL;
    SSL *q = NULL;

    *ctx = NULL;
    memset(&dir, 0, sizeof dir);
    dir.sin_family = AF_INET;
    dir.sin_port = htons((uint16_t)puerto); /* orden de bytes de red */
    if (inet_pton(AF_INET, ip, &dir.sin_addr) != 1) {
        fprintf(stderr, "direccion IPv4 no valida: %s\n", ip);
        return NULL;
    }
    /* QUIC va sobre UDP: el socket es de datagramas, no SOCK_STREAM. */
    *sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (*sock < 0) {
        perror("socket");
        return NULL;
    }

    /* Contexto de cliente QUIC (solo admite TLS 1.3). */
    *ctx = SSL_CTX_new(OSSL_QUIC_client_method());
    if (!*ctx) {
        error_ssl(prefijo, "SSL_CTX_new");
        goto fallo;
    }
    SSL_CTX_set_keylog_callback(*ctx, guardar_clave);
    /* Solo se acepta a un broker que presente el certificado bono-cert.pem. */
    SSL_CTX_set_verify(*ctx, SSL_VERIFY_PEER, NULL);
    if (!SSL_CTX_load_verify_file(*ctx, CERT)) {
        error_ssl(prefijo, "falta " CERT " (se crea con make)");
        goto fallo;
    }

    q = SSL_new(*ctx);
    destino = BIO_ADDR_new();
    if (!q || !destino || !SSL_set_fd(q, *sock) ||
        !BIO_ADDR_rawmake(destino, AF_INET, &dir.sin_addr, sizeof dir.sin_addr, dir.sin_port) ||
        !SSL_set1_initial_peer_addr(q, destino) ||           /* a donde va el primer datagrama */
        SSL_set_alpn_protos(q, (const unsigned char *)ALPN, 5)) { /* esta devuelve 0 si va bien */
        error_ssl(prefijo, "no se pudo preparar la conexion");
        goto fallo;
    }
    /* Tiempo sin noticias del broker tras el cual QUIC da la conexion por muerta. */
    if (!SSL_set_feature_request_uint(q, SSL_VALUE_QUIC_IDLE_TIMEOUT, INACTIVIDAD_MS))
        ERR_clear_error();
    if (SSL_connect(q) != 1) { /* handshake QUIC + TLS 1.3; bloquea hasta que termina o falla */
        error_ssl(prefijo, "no se pudo conectar con el broker");
        goto fallo;
    }
    BIO_ADDR_free(destino);
    fprintf(stderr, "%s[quic] conectado: %s, %s\n", prefijo, SSL_get_version(q), SSL_get_cipher_name(q));
    return q;

fallo:
    BIO_ADDR_free(destino); /* las tres aceptan NULL */
    SSL_free(q);
    SSL_CTX_free(*ctx);
    *ctx = NULL;
    if (close(*sock) < 0)
        perror("close");
    return NULL;
}

/* Duerme hasta que llegue un datagrama o hasta que QUIC necesite actuar (retransmitir, etc.). */
static void dormir(SSL *q, int sock)
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
        perror("poll");
}

/* Un suscriptor completo: conecta, se suscribe y muestra lo que llega hasta Ctrl-C. */
static int suscriptor(const char *prefijo, const char *ip, long puerto, char **temas, int n_temas)
{
    char entrada[MAX_LINEA], salida[MAX_LINEA + 32], *salto;
    size_t n_entrada = 0, resto, leido;
    SSL_CTX *ctx;
    SSL *q;
    int sock, i, n, estado = EXIT_SUCCESS;

    q = conectar(prefijo, ip, puerto, &ctx, &sock);
    if (!q)
        return EXIT_FAILURE;

    /* El primer SSL_write_ex() abre el stream. Bloquea hasta que QUIC acepta la linea. */
    for (i = 0; i < n_temas; i++) {
        n = snprintf(salida, sizeof salida, "S %s\n", temas[i]);
        if (!SSL_write_ex(q, salida, (size_t)n, &leido)) {
            error_ssl(prefijo, "no se pudo enviar la suscripcion");
            estado = EXIT_FAILURE;
            fin = 1;
        }
    }

    /* A partir de aqui, sin bloquear: asi se puede atender a Ctrl-C mientras se espera. */
    if (!SSL_set_blocking_mode(q, 0))
        error_ssl(prefijo, "SSL_set_blocking_mode");
    while (!fin) {
        if (!SSL_read_ex(q, entrada + n_entrada, sizeof entrada - n_entrada, &leido)) {
            if (SSL_get_error(q, 0) == SSL_ERROR_WANT_READ) { /* no hay datos todavia */
                ERR_clear_error();
                dormir(q, sock);
                continue;
            }
            fprintf(stderr, "%sel broker cerro la conexion\n", prefijo);
            ERR_clear_error();
            break;
        }
        n_entrada += leido;

        while ((salto = memchr(entrada, '\n', n_entrada))) {
            resto = (size_t)(salto + 1 - entrada);
            n = snprintf(salida, sizeof salida, "%s%.*s", prefijo, (int)resto, entrada);
            if (escribir_todo(STDOUT_FILENO, salida, (size_t)n) < 0)
                fin = 1; /* nadie lee la salida */
            n_entrada -= resto;
            memmove(entrada, salto + 1, n_entrada);
        }
        if (n_entrada == sizeof entrada) /* linea mas larga que el buffer: se descarta */
            n_entrada = 0;
    }

    /* Cierre ordenado: FIN del stream, esperar el FIN del broker y cerrar la conexion. */
    if (!SSL_set_blocking_mode(q, 1))
        ERR_clear_error();
    if (!SSL_stream_conclude(q, 0))
        ERR_clear_error();
    while (SSL_read_ex(q, entrada, sizeof entrada, &leido))
        ;
    while (SSL_shutdown(q) == 0)
        ;
    SSL_free(q);
    SSL_CTX_free(ctx);
    if (close(sock) < 0) {
        perror("close");
        estado = EXIT_FAILURE;
    }
    return estado;
}

int main(int argc, char **argv)
{
    static char lineas[MAX_SUSCRIPTORES][MAX_TEMAS * MAX_TEMA];
    static char *temas[MAX_SUSCRIPTORES][MAX_TEMAS];
    char ip[64], prefijo[32], texto[96];
    struct sigaction sa;
    long puerto, n;
    pid_t hijos[MAX_SUSCRIPTORES];
    int n_temas[MAX_SUSCRIPTORES], i, j, estado, salida = EXIT_SUCCESS;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = al_recibir_senal; /* sin SA_RESTART: interrumpe el recv() en curso */
    if (sigemptyset(&sa.sa_mask) < 0 || sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        morir("sigaction");

    if (argc >= 4) { /* un suscriptor, todo por argumentos */
        if (!leer_entero(argv[2], 1, 65535, &puerto) || argc - 3 > MAX_TEMAS) {
            fprintf(stderr, "argumentos no validos\nuso: %s <ip> <puerto> <tema>...\n", argv[0]);
            return EXIT_FAILURE;
        }
        for (i = 3; i < argc; i++)
            if (strlen(argv[i]) >= MAX_TEMA || strcspn(argv[i], " \t") != strlen(argv[i]) || !argv[i][0]) {
                fprintf(stderr, "tema no valido: \"%s\"\n", argv[i]);
                return EXIT_FAILURE;
            }
        return suscriptor("", argv[1], puerto, argv + 3, argc - 3);
    }
    if (argc != 1) {
        fprintf(stderr, "uso: %s                           (pregunta los datos)\n"
                        "     %s <ip> <puerto> <tema>...\n", argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    /* Modo interactivo. */
    printf("== Suscriptor QUIC ==\n");
    preguntar("IP del broker", "127.0.0.1", ip, sizeof ip);
    puerto = preguntar_numero("Puerto del broker", 9000, 1, 65535);
    n = preguntar_numero("Cantidad de suscriptores", 2, 1, MAX_SUSCRIPTORES);
    for (i = 0; i < n; i++) {
        do {
            snprintf(texto, sizeof texto, "Temas del suscriptor %d (separados por espacios)", i + 1);
            preguntar(texto, i % 2 ? "C-D" : "A-B C-D", lineas[i], sizeof lineas[i]);
            n_temas[i] = partir_temas(lineas[i], temas[i]);
        } while (n_temas[i] == 0 && printf("  entre 1 y %d temas de menos de %d letras\n", MAX_TEMAS, MAX_TEMA));
    }
    printf("Esperando mensajes (Ctrl-C para salir)...\n");

    if (n == 1) /* uno solo: no hace falta otro proceso */
        return suscriptor("", ip, puerto, temas[0], n_temas[0]);

    fflush(stdout); /* para que los hijos no hereden texto pendiente de imprimir */
    for (i = 0; i < n; i++) {
        hijos[i] = fork();
        if (hijos[i] < 0)
            morir("fork");
        if (hijos[i] == 0) {
            snprintf(prefijo, sizeof prefijo, "[sub %d] ", i + 1);
            exit(suscriptor(prefijo, ip, puerto, temas[i], n_temas[i]));
        }
    }
    /* Ctrl-C llega a todo el grupo: cada hijo cierra su conexion y el padre los espera. */
    for (i = 0; i < n;) {
        if (wait(&estado) < 0) {
            if (errno != EINTR)
                morir("wait");
            /* La senal pudo llegar solo al padre: se reenvia para que todos cierren. */
            for (j = 0; j < n; j++)
                if (kill(hijos[j], SIGTERM) < 0 && errno != ESRCH)
                    perror("kill");
            continue;
        }
        if (!WIFEXITED(estado) || WEXITSTATUS(estado) != EXIT_SUCCESS)
            salida = EXIT_FAILURE;
        i++;
    }
    return salida;
}
