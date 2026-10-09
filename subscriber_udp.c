#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define MAX_DATAGRAMA 1472 /* 1500 (MTU Ethernet) - 20 (IP) - 8 (UDP), como en el broker */
#define BUFFER_RX (1 << 20) /* buffer de recepcion que se pide al nucleo */
#define MAX_TEMA 64
#define MAX_TEMAS 16   /* temas por suscriptor */
#define MAX_SUSCRIPTORES 32

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

/* Envia un datagrama: en UDP sale entero o falla. Devuelve 0 o -1. */
static int enviar_datagrama(int fd, const char *datos, size_t len)
{
    while (send(fd, datos, len, 0) < 0) {
        if (errno == EINTR)
            continue;
        perror("send");
        return -1;
    }
    return 0;
}

/* Crea el socket UDP y fija el broker como unico destino. Devuelve el descriptor o -1. */
static int conectar(const char *ip, long puerto)
{
    struct sockaddr_in dir;
    int fd, pedido = BUFFER_RX;

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

    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &pedido, sizeof pedido) < 0)
        perror("setsockopt SO_RCVBUF");

    if (connect(fd, (struct sockaddr *)&dir, sizeof dir) < 0) {
        perror("connect");
        if (close(fd) < 0)
            perror("close");
        return -1;
    }
    return fd;
}

/* Un suscriptor completo: crea su socket, se suscribe y muestra lo que llega hasta Ctrl-C. */
static int suscriptor(const char *prefijo, const char *ip, long puerto, char **temas, int n_temas)
{
    char datos[MAX_DATAGRAMA + 1], salida[MAX_DATAGRAMA + 34];
    ssize_t r;
    int fd, i, n, estado = EXIT_SUCCESS;

    fd = conectar(ip, puerto);
    if (fd < 0)
        return EXIT_FAILURE;

    /* Una suscripcion por datagrama. El broker no responde: no se sabe si llego. */
    for (i = 0; i < n_temas; i++) {
        n = snprintf(salida, sizeof salida, "S %s\n", temas[i]);
        if (enviar_datagrama(fd, salida, (size_t)n) < 0) {
            estado = EXIT_FAILURE;
            fin = 1;
        }
    }

    while (!fin) {
        /* Cada recv() entrega un datagrama entero, es decir, un mensaje completo. */
        r = recv(fd, datos, MAX_DATAGRAMA, MSG_TRUNC);
        if (r < 0) {
            if (errno == EINTR)
                continue; /* si fue Ctrl-C, el bucle termina al mirar fin */
            if (errno == ECONNREFUSED)
                fprintf(stderr, "%sno hay broker en ese puerto (ICMP port unreachable)\n", prefijo);
            else
                perror("recv");
            estado = EXIT_FAILURE;
            break;
        }
        if (r > MAX_DATAGRAMA) { /* MSG_TRUNC: el tamano real no cabia en el buffer */
            fprintf(stderr, "%sdatagrama de %zd bytes recortado: descartado\n", prefijo, r);
            continue;
        }
        if (r == 0)
            continue; /* datagrama vacio: valido en UDP, no significa cierre */
        if (datos[r - 1] != '\n')
            datos[r++] = '\n';
        /* Una sola llamada a write() por mensaje: no se mezclan varios suscriptores. */
        n = snprintf(salida, sizeof salida, "%s%.*s", prefijo, (int)r, datos);
        if (escribir_todo(STDOUT_FILENO, salida, (size_t)n) < 0)
            fin = 1; /* nadie lee la salida */
    }

    /* Baja: sin esto el broker seguiria enviando a este puerto para siempre. */
    if (estado == EXIT_SUCCESS && enviar_datagrama(fd, "U\n", 2) < 0)
        estado = EXIT_FAILURE;
    if (close(fd) < 0) {
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
    printf("== Suscriptor UDP ==\n");
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
    /* Ctrl-C llega a todo el grupo: cada hijo se da de baja y el padre los espera. */
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
