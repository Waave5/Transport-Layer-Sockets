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

#define MAX_LINEA 1024  /* tamano maximo de un mensaje; menos de 1472 para no fragmentar */
#define MAX_TEMA 64
#define MAX_PUBLICADORES 32
#define PAUSA_MS 20     /* pausa entre eventos de ejemplo */

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

/*
 * Envia un datagrama. En UDP send() no parte el mensaje: sale entero o falla. Devuelve
 * 0 o -1. El socket esta conectado, asi que un error asincrono (el ICMP "port
 * unreachable" que provoca un datagrama anterior si no hay broker) aparece aqui.
 */
static int enviar_datagrama(int fd, const char *datos, size_t len)
{
    while (send(fd, datos, len, 0) < 0) {
        if (errno == EINTR && !fin)
            continue;
        if (errno == ECONNREFUSED)
            fprintf(stderr, "send: no hay broker en ese puerto (ICMP port unreachable)\n");
        else
            perror("send");
        return -1;
    }
    return 0;
}

/* Crea el socket UDP y fija el broker como unico destino. Devuelve el descriptor o -1. */
static int conectar(const char *ip, long puerto)
{
    struct sockaddr_in dir;
    int fd;

    memset(&dir, 0, sizeof dir);
    dir.sin_family = AF_INET;
    dir.sin_port = htons((uint16_t)puerto); /* orden de bytes de red */
    if (inet_pton(AF_INET, ip, &dir.sin_addr) != 1) {
        fprintf(stderr, "direccion IPv4 no valida: %s\n", ip);
        return -1;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    
    if (connect(fd, (struct sockaddr *)&dir, sizeof dir) < 0) {
        perror("connect");
        if (close(fd) < 0)
            perror("close");
        return -1;
    }
    return fd;
}

/* Arma "P <tema> <seq> <texto>\n", lo envia en un datagrama y lo muestra. Devuelve 0 o -1. */
static int publicar(int fd, const char *prefijo, const char *tema, int seq, const char *texto)
{
    char linea[MAX_LINEA];
    int n = snprintf(linea, sizeof linea, "P %s %d %s\n", tema, seq, texto);

    if (n >= (int)sizeof linea) { /* no cabia: se recorta y se conserva el fin de linea */
        n = (int)sizeof linea - 1;
        linea[n - 1] = '\n';
        fprintf(stderr, "%saviso: mensaje %d recortado a %d bytes\n", prefijo, seq, n);
    }
    if (enviar_datagrama(fd, linea, (size_t)n) < 0)
        return -1;
    printf("%senviado: %.*s\n", prefijo, n - 3, linea + 2);
    return 0;
}

static int publicador(const char *prefijo, const char *ip, long puerto, const char *tema, long cuantos)
{
    char texto[MAX_LINEA];
    struct timespec pausa = {0, PAUSA_MS * 1000000L};
    socklen_t tam;
    int fd, seq, pendiente = 0, estado = EXIT_SUCCESS;

    fd = conectar(ip, puerto);
    if (fd < 0)
        return EXIT_FAILURE;

    if (cuantos < 0) {
        for (seq = 1; !fin && fgets(texto, sizeof texto, stdin); seq++) {
            texto[strcspn(texto, "\r\n")] = '\0';
            if (publicar(fd, prefijo, tema, seq, texto) < 0) {
                estado = EXIT_FAILURE;
                break;
            }
        }
    } else {
        for (seq = 1; !fin && seq <= cuantos; seq++) {
            snprintf(texto, sizeof texto, "%s al minuto %d", eventos[(seq - 1) % N_EVENTOS], 1 + (seq * 7) % 90);
            if (publicar(fd, prefijo, tema, seq, texto) < 0) {
                estado = EXIT_FAILURE;
                break;
            }
            nanosleep(&pausa, NULL); /* si una senal la interrumpe, el bucle mira fin */
        }
    }

    nanosleep(&pausa, NULL);
    tam = sizeof pendiente;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &pendiente, &tam) < 0)
        perror("getsockopt SO_ERROR");
    if (pendiente == ECONNREFUSED && estado == EXIT_SUCCESS) {
        fprintf(stderr, "%sno hay broker en ese puerto (ICMP port unreachable)\n", prefijo);
        estado = EXIT_FAILURE;
    }
    if (close(fd) < 0) {
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

    
    printf("== Publicador UDP ==\n");
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

    if (n == 1) { 
        if (mensajes[0] == 0)
            printf("Escribe un evento por linea (Ctrl-D para terminar):\n");
        return publicador("", ip, puerto, temas[0], mensajes[0] ? mensajes[0] : -1);
    }

    fflush(stdout); 
    for (i = 0; i < n; i++) {
        hijos[i] = fork();
        if (hijos[i] < 0)
            morir("fork");
        if (hijos[i] == 0) {
            snprintf(prefijo, sizeof prefijo, "[pub %d] ", i + 1);
            exit(publicador(prefijo, ip, puerto, temas[i], mensajes[i]));
        }
    }
    for (i = 0; i < n;) { 
        if (wait(&estado) < 0) {
            if (errno != EINTR)
                morir("wait");
            
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
