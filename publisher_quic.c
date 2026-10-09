#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#define MAX_LINEA 1024  /* tamano maximo de una linea, con su '\n' (el mismo del broker) */
#define MAX_TEMA 64
#define MAX_PUBLICADORES 32
#define PAUSA_MS 20     /* pausa entre eventos de ejemplo */

#define INACTIVIDAD_MS 30000 /* sin respuesta del broker durante este tiempo, la conexion se da por muerta */
#define ALPN "\4neko"        /* un byte de longitud y el nombre del protocolo de aplicacion */
#define CERT "bono-cert.pem"

static const char *eventos[] = {
    "Gol de Equipo A",
    "Tarjeta amarilla al numero 10 de Equipo B",
    "Cambio: jugador 10 entra por jugador 20",
    "Tiro de esquina para Equipo B",
    "Fuera de lugar de Equipo A",
    "Gol de Equipo B",
    "Tarjeta roja al numero 4 de Equipo A",
    "Atajada del portero de Equipo B",
};
#define N_EVENTOS ((int)(sizeof eventos / sizeof eventos[0]))

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

/* Un tema es una palabra sin espacios, por ejemplo A-B. */
static int tema_valido(const char *tema)
{
    size_t n = strlen(tema);

    return n > 0 && n < MAX_TEMA && strcspn(tema, " \t") == n;
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

/*
 * Abre la conexion QUIC con el broker: socket UDP, contexto TLS, objeto de conexion y
 * handshake. Devuelve la conexion (y deja en *ctx y *sock lo que hay que liberar al
 * final) o NULL si algo falla.
 */
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

/* Arma "P <tema> <seq> <texto>\n", lo envia y lo muestra. Devuelve 0 o -1. */
static int publicar(SSL *q, const char *prefijo, const char *tema, int seq, const char *texto)
{
    char linea[MAX_LINEA];
    size_t escrito;
    int n = snprintf(linea, sizeof linea, "P %s %d %s\n", tema, seq, texto);

    if (n >= (int)sizeof linea) { /* no cabia: se recorta y se conserva el fin de linea */
        n = (int)sizeof linea - 1;
        linea[n - 1] = '\n';
        fprintf(stderr, "%saviso: mensaje %d recortado a %d bytes\n", prefijo, seq, n);
    }
    /* En modo bloqueante SSL_write_ex() vuelve cuando QUIC ha aceptado la linea entera. */
    if (!SSL_write_ex(q, linea, (size_t)n, &escrito)) {
        error_ssl(prefijo, "no se pudo enviar: conexion perdida");
        return -1;
    }
    printf("%senviado: %.*s\n", prefijo, n - 3, linea + 2);
    return 0;
}

/*
 * Un publicador completo: conecta, publica y cierra. cuantos > 0 envia eventos de
 * ejemplo; cuantos < 0 publica cada linea de stdin. Devuelve el codigo de salida.
 */
static int publicador(const char *prefijo, const char *ip, long puerto, const char *tema, long cuantos)
{
    char texto[MAX_LINEA], basura[256];
    struct timespec pausa = {0, PAUSA_MS * 1000000L};
    SSL_CTX *ctx;
    SSL *q;
    size_t leido;
    int sock, seq, estado = EXIT_SUCCESS;

    q = conectar(prefijo, ip, puerto, &ctx, &sock);
    if (!q)
        return EXIT_FAILURE;

    if (cuantos < 0) {
        for (seq = 1; !fin && fgets(texto, sizeof texto, stdin); seq++) {
            texto[strcspn(texto, "\r\n")] = '\0';
            if (publicar(q, prefijo, tema, seq, texto) < 0) {
                estado = EXIT_FAILURE;
                break;
            }
        }
    } else {
        for (seq = 1; !fin && seq <= cuantos; seq++) {
            snprintf(texto, sizeof texto, "%s al minuto %d", eventos[(seq - 1) % N_EVENTOS], 1 + (seq * 7) % 90);
            if (publicar(q, prefijo, tema, seq, texto) < 0) {
                estado = EXIT_FAILURE;
                break;
            }
            nanosleep(&pausa, NULL); /* si una senal la interrumpe, el bucle mira fin */
        }
    }

    /*
     * Cierre ordenado. SSL_stream_conclude() envia el FIN del stream: "no voy a mandar
     * mas". Se lee hasta ver el FIN del broker, que llega cuando ya leyo todo, y
     * entonces SSL_shutdown() cierra la conexion (trama CONNECTION_CLOSE).
     */
    if (!SSL_stream_conclude(q, 0))
        ERR_clear_error();
    while (SSL_read_ex(q, basura, sizeof basura, &leido))
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
    static char temas[MAX_PUBLICADORES][MAX_TEMA];
    char ip[64], prefijo[32], texto[96];
    struct sigaction sa;
    long puerto, cuantos = -1, n, mensajes[MAX_PUBLICADORES];
    pid_t hijos[MAX_PUBLICADORES];
    int i, j, estado, salida = EXIT_SUCCESS;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = al_recibir_senal; /* sin SA_RESTART: interrumpe la llamada en curso */
    if (sigemptyset(&sa.sa_mask) < 0 || sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        morir("sigaction");

    if (argc == 4 || argc == 5) { /* un publicador, todo por argumentos */
        if (!leer_entero(argv[2], 1, 65535, &puerto) || !tema_valido(argv[3]) ||
            (argc == 5 && !leer_entero(argv[4], 1, 1000000, &cuantos))) {
            fprintf(stderr, "argumentos no validos\nuso: %s <ip> <puerto> <tema> [n]\n", argv[0]);
            return EXIT_FAILURE;
        }
        return publicador("", argv[1], puerto, argv[3], cuantos);
    }
    if (argc != 1) {
        fprintf(stderr, "uso: %s                           (pregunta los datos)\n"
                        "     %s <ip> <puerto> <tema> [n]\n", argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    /* Modo interactivo. */
    printf("== Publicador QUIC ==\n");
    preguntar("IP del broker", "127.0.0.1", ip, sizeof ip);
    puerto = preguntar_numero("Puerto del broker", 9000, 1, 65535);
    n = preguntar_numero("Cantidad de publicadores", 2, 1, MAX_PUBLICADORES);
    for (i = 0; i < n; i++) {
        do {
            snprintf(texto, sizeof texto, "Tema (partido) del publicador %d", i + 1);
            preguntar(texto, i % 2 ? "C-D" : "A-B", temas[i], sizeof temas[i]);
        } while (!tema_valido(temas[i]) && printf("  el tema es una palabra sin espacios\n"));
        snprintf(texto, sizeof texto, "Mensajes del publicador %d%s", i + 1,
                 n == 1 ? " (0 = escribirlos a mano)" : "");
        mensajes[i] = preguntar_numero(texto, 10, n == 1 ? 0 : 1, 1000000);
    }

    if (n == 1) { /* uno solo: no hace falta otro proceso */
        if (mensajes[0] == 0)
            printf("Escribe un evento por linea (Ctrl-D para terminar):\n");
        return publicador("", ip, puerto, temas[0], mensajes[0] ? mensajes[0] : -1);
    }

    fflush(stdout); /* para que los hijos no hereden texto pendiente de imprimir */
    for (i = 0; i < n; i++) {
        hijos[i] = fork();
        if (hijos[i] < 0)
            morir("fork");
        if (hijos[i] == 0) {
            snprintf(prefijo, sizeof prefijo, "[pub %d] ", i + 1);
            exit(publicador(prefijo, ip, puerto, temas[i], mensajes[i]));
        }
    }
    for (i = 0; i < n;) { /* el padre solo espera a que terminen todos */
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
