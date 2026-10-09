# Laboratorio 3: publicación–suscripción sobre TCP, UDP y QUIC

Sistema de noticias deportivas en tiempo real: los publicadores envían eventos de un
partido, el broker los reenvía a los suscriptores de ese partido. Hay una versión por
protocolo de transporte, cada programa en su propio archivo C:

| | Broker | Publicador | Suscriptor |
|---|---|---|---|
| **TCP** | `broker_tcp.c` | `publisher_tcp.c` | `subscriber_tcp.c` |
| **UDP** | `broker_udp.c` | `publisher_udp.c` | `subscriber_udp.c` |
| **QUIC** (bono) | `broker_quic.c` | `publisher_quic.c` | `subscriber_quic.c` |

Los seis de TCP y UDP solo usan libc y las llamadas de sockets del sistema. Los tres de
QUIC usan además la librería **OpenSSL** (3.5 o posterior), que es la que implementa
QUIC; cada función que se usa de ella está explicada en [`docs/quic.md`](docs/quic.md).

Cada archivo es autosuficiente (no hay cabeceras propias) y está comentado función por
función.


## Compilar

```sh
make all
```

Sin `make`, cada archivo compila por sí solo:

```sh
gcc broker_tcp.c -o broker_tcp
gcc broker_quic.c -o broker_quic -lssl -lcrypto     # los de QUIC enlazan con OpenSSL
```

`make all` crea también `bono-cert.pem` y `bono-key.pem`, el certificado y la clave que
necesita el broker QUIC.


## Usar

Tres terminales, en este orden: broker, suscriptores, publicadores. Los comandos son
iguales para los tres protocolos cambiando `tcp` por `udp` o `quic`.

**Terminal 1: el broker.** El puerto es opcional (9000 por defecto).

```sh
./broker_tcp 9000
```

**Terminal 2: los suscriptores.** Sin argumentos, el programa pregunta los datos; Enter
acepta el valor entre corchetes.

```
$ ./subscriber_tcp
== Suscriptor TCP ==
IP del broker [127.0.0.1]:
Puerto del broker [9000]:
Cantidad de suscriptores [2]:
Temas del suscriptor 1 (separados por espacios) [A-B C-D]:
Temas del suscriptor 2 (separados por espacios) [C-D]:
Esperando mensajes (Ctrl-C para salir)...
[sub 1] A-B 1 Gol de Equipo A al minuto 8
[sub 2] C-D 1 Gol de Equipo A al minuto 8
[sub 1] C-D 1 Gol de Equipo A al minuto 8
```

**Terminal 3: los publicadores.**

```
$ ./publisher_tcp
== Publicador TCP ==
IP del broker [127.0.0.1]:
Puerto del broker [9000]:
Cantidad de publicadores [2]:
Tema (partido) del publicador 1 [A-B]:
Mensajes del publicador 1 [10]:
Tema (partido) del publicador 2 [C-D]:
Mensajes del publicador 2 [10]:
[pub 1] enviado: A-B 1 Gol de Equipo A al minuto 8
[pub 2] enviado: C-D 1 Gol de Equipo A al minuto 8
```

Cada suscriptor y cada publicador es un proceso distinto con su propio socket, así que
el broker y Wireshark los ven como clientes independientes. Con un solo publicador se
puede contestar 0 mensajes para escribir los eventos a mano, uno por línea.

Un tema es una palabra sin espacios que identifica el partido, por ejemplo `A-B`.

### Con argumentos

Para lanzar un solo cliente sin preguntas (es lo que usan las pruebas):

```sh
./subscriber_tcp 127.0.0.1 9000 A-B C-D         # un suscriptor, uno o varios temas
./publisher_tcp 127.0.0.1 9000 A-B 10           # un publicador, 10 eventos de ejemplo
echo "Gol de Equipo A al minuto 32" | ./publisher_tcp 127.0.0.1 9000 A-B   # eventos por stdin
```

Si leyeron hasta este punto podemos afirmar que flechas tiene una sola neurona algunos días.

### Lo que muestra el broker

```
[*] broker TCP en el puerto 9000 (Ctrl-C para salir)
[+] cliente 1 conectado
[S] cliente 1 se suscribe a A-B
[+] cliente 2 conectado
[P] A-B 1 Gol de Equipo A al minuto 8 -> 1 suscriptores
[-] cliente 2 desconectado (cerro la conexion)
```

En UDP no hay `[+]`, porque no hay conexiones: el suscriptor aparece con su IP y su
puerto al suscribirse, y `[-]` solo sale si avisa de que se va.

### QUIC

Los tres programas de QUIC hay que lanzarlos desde la carpeta donde están
`bono-cert.pem` y `bono-key.pem`. Al conectar, cada cliente lo anuncia:

```
[quic] conectado: QUICv1, TLS_AES_256_GCM_SHA384
```

Para que Wireshark pueda descifrar una captura, se lanzan con la variable
`SSLKEYLOGFILE`; cada proceso añade ahí sus claves de sesión:

```sh
SSLKEYLOGFILE=claves.log ./broker_quic 9000
```


## Probar

```sh
./try.sh
```

Para cada protocolo levanta un broker, dos suscriptores y dos publicadores con 10
mensajes cada uno, y comprueba que cada suscriptor recibe exactamente los mensajes de
sus temas y en orden, y que todos los procesos cierran limpiamente.


## Protocolo de aplicación

Texto, una línea por mensaje, igual en los tres transportes:

    S <tema>\n                  suscribirse
    P <tema> <seq> <texto>\n    publicar
    U\n                         darse de baja (solo UDP)

El broker reenvía `<tema> <seq> <texto>\n` sin modificarlo a los suscritos a ese tema.
`<seq>` lo numera cada publicador desde 1: es lo que deja ver en el suscriptor una
pérdida o un desorden. Una línea mide como mucho 1024 bytes.


## Cómo está hecho cada uno

### TCP

- **Broker de un hilo con `poll()`**. El socket de escucha solo hace `accept()`; los
  datos van por el descriptor de cada conexión.
- **Reensamblado de líneas**. TCP entrega bytes, no mensajes: un `recv()` puede traer
  media línea o varias. Broker y suscriptor acumulan hasta ver el `\n`.
- **Envíos que no bloquean**. Lo que el núcleo no acepta se guarda en una cola por
  cliente (64 KiB) y sale cuando `poll()` avisa. Si la cola se llena, ese suscriptor no
  está leyendo y se le desconecta, para que no frene a los demás.
- **Cierre ordenado**. El publicador hace `shutdown(SHUT_WR)`, espera el FIN del broker
  y entonces cierra: nada de lo último que envió se pierde.

### UDP

- **Un solo socket en el broker**, sin `listen()` ni `accept()`. Recibe con
  `recvfrom()`, que dice quién envió cada datagrama, y reenvía con `sendto()`.
- **El suscriptor es su IP y su puerto**. El broker no se entera si desaparece; por eso
  al salir envía `U`.
- **Un mensaje por datagrama**, de 1472 bytes como máximo para que no se fragmente.
- **Clientes con socket conectado**. `connect()` en UDP no envía nada, pero hace que el
  núcleo descarte datagramas de otro origen y entregue los errores ICMP: si no hay
  broker, el segundo envío falla con "no hay broker en ese puerto".
- **Nada se confirma ni se reenvía**. Si se pierde el `S`, el suscriptor no recibe nada y
  nadie avisa. Si se pierde un mensaje, se ve como un salto en `<seq>`.

### QUIC

Mismo sistema y mismo protocolo de aplicación, sobre QUIC (RFC 9000): datagramas UDP por
debajo, TLS 1.3 obligatorio y, por cliente, una conexión con un stream fiable y
ordenado. La implementación de QUIC es la de OpenSSL; los programas solo crean el socket
UDP y llaman a la librería. [`docs/quic.md`](docs/quic.md) lo explica paso a paso.

- **Broker de un hilo**, todo en modo no bloqueante.
- **Contrapresión**: si el stream de un suscriptor está lleno, el broker deja de leer a
  su publicador hasta que quepa, en vez de perder el mensaje.
- **Cierre ordenado**: FIN del stream en los dos sentidos y después `CONNECTION_CLOSE`.

En QUIC no hay SYN ni FIN como banderas de cabecera: el establecimiento son los paquetes
`Initial` y `Handshake`, y los ACK son tramas cifradas.


## Buenas prácticas aplicadas

| Práctica | Dónde |
|---|---|
| Se comprueba el retorno de toda llamada al sistema (`perror`) | Los nueve archivos |
| `memset` de `sockaddr_in`; `htons`/`htonl` al llenar, `ntohs` al mostrar | Los nueve |
| `inet_pton` en vez de `inet_addr`: una IP mal escrita se rechaza | Publicadores y suscriptores |
| `snprintf` en vez de `strcat`/`sprintf` | Los nueve |
| `memchr`/`memcpy`/`memmove` sobre los datos de red, no funciones de cadena | Brokers y suscriptores |
| Líneas con bytes nulos, demasiado largas o mal formadas se descartan | Los tres brokers |
| `SO_REUSEADDR` en el servidor | `broker_tcp.c`, `broker_udp.c` |
| `TCP_NODELAY`: mensajes cortos en tiempo real, cada uno en su segmento | Los tres de TCP |
| `SO_KEEPALIVE`: detectar un extremo que desapareció sin cerrar | `broker_tcp.c`, `subscriber_tcp.c` |
| `SO_SNDTIMEO`: ningún envío bloquea para siempre (y `SO_RCVTIMEO` en la espera del cierre) | `publisher_tcp.c`, `subscriber_tcp.c` |
| `SO_RCVBUF` de 1 MiB, porque UDP no tiene control de flujo | `broker_udp.c`, `subscriber_udp.c`, `broker_quic.c` |
| El socket de escucha solo se usa para `accept()` | `broker_tcp.c` |
| Lecturas y escrituras parciales en bucle; `EINTR` y `EAGAIN` tratados | Los de TCP |
| `recvfrom`/`sendto` en el servidor; socket conectado en los clientes | Los de UDP |
| Datagrama recortado detectado con `MSG_TRUNC` | `broker_udp.c`, `subscriber_udp.c` |
| Errores ICMP asíncronos reportados | `publisher_udp.c`, `subscriber_udp.c` |
| `shutdown(SHUT_WR)` antes de `close()` | `publisher_tcp.c`, `subscriber_tcp.c` |
| Ctrl-C cierra todos los descriptores antes de salir | Los nueve |
| Sin memoria dinámica propia: tablas estáticas, nada que liberar por mensaje | Los nueve |
| Tiempo de inactividad de QUIC fijado a 30 s | Los tres de QUIC |

Se compilan sin avisos con `-Wall -Wextra -pedantic` en gcc y clang.

### Lo que no se aplicó, y por qué

| Técnica | Motivo |
|---|---|
| Hilos, thread pool, `epoll` edge-triggered | Son para cientos o miles de conexiones. Con un hilo y `poll()` el broker atendió 10 000 mensajes de 5 publicadores sin perder ninguno |
| `TCP_CORK` | Agrupa escrituras: lo contrario de `TCP_NODELAY`, que es lo que conviene a mensajes en tiempo real |
| `SO_REUSEPORT` | Solo sirve con varios procesos aceptando en el mismo puerto |
| `sendfile`, `splice`, `writev` | No se transfieren archivos y el mensaje se reenvía desde el mismo buffer en que llegó, sin copiarlo |
| `TCP_FASTOPEN`, 0-RTT de QUIC | Ahorran un viaje al reconectar; aquí las conexiones son largas |
| UDP GSO, multicast, `IP_MTU_DISCOVER` | Los mensajes miden menos que un datagrama y el reparto lo decide el broker por tema |
| Ajustar `net.core.rmem_max` con `sysctl` | Es configuración del sistema, no del programa; el broker UDP muestra el buffer que realmente obtuvo |


## Qué se probó

`./try.sh` cubre el escenario del enunciado. Además, con los nueve programas:

| Prueba | TCP | UDP | QUIC |
|---|---|---|---|
| 5 publicadores × 2000 mensajes sin pausa | 10 000 de 10 000, en orden | unos 3500 de 10 000 | 10 000 de 10 000, en orden |
| 50 mensajes con 20 % de pérdida simulada | 50, en orden | 34, con saltos | 50, en orden |
| Broker apagado al arrancar el cliente | Falla al instante | Falla en el segundo envío | Falla a los 30 s |
| Broker detenido con un suscriptor conectado | El suscriptor termina | No se entera | El suscriptor termina |
| AddressSanitizer y UBSan | Sin errores | Sin errores | Sin errores |

Solo en TCP:

- **Suscriptor congelado** mientras llegan 30 000 mensajes: el broker lo desconecta y el
  otro suscriptor recibe los 30 000.
- **Entrada hostil** (byte a byte, bytes nulos, una línea de 5000 bytes, basura
  aleatoria): el broker descarta lo inválido y sigue entregando lo válido.

La pérdida de UDP a toda velocidad ocurre en el buffer de recepción del broker: los
publicadores envían más rápido de lo que él lee y UDP no tiene forma de frenarlos. Con
la pausa de 20 ms de los eventos de ejemplo no se pierde nada en local.


## Capturas para el informe

En `capturas/` hay una por protocolo, con el escenario del enunciado (1 broker,
2 suscriptores, 2 publicadores de 10 mensajes), tomadas en la interfaz de loopback:

| Archivo | Paquetes | Bytes | Contenido |
|---|---|---|---|
| `capturas/tcp_pubsub.pcap` | 130 | 11 142 | 4 conexiones: 4 SYN, 8 FIN, 53 segmentos con datos, 61 ACK sin datos |
| `capturas/udp_pubsub.pcap` | 55 | 4 812 | 3 `S`, 20 `P`, 30 reenvíos y 2 `U`: un datagrama por mensaje, nada más |
| `capturas/quic_pubsub.pcap` | 156 | 68 888 | 4 conexiones; los primeros paquetes van rellenos a 1200 bytes |
| `capturas/quic_claves.log` | | | Claves de sesión para descifrar la anterior en Wireshark |

Para tomar otras (o capturar con Wireshark en la interfaz `lo`):

```sh
make pcap-tcp     # sudo tcpdump -i lo -w tcp_pubsub.pcap tcp port 9000
make pcap-udp     # sudo tcpdump -i lo -w udp_pubsub.pcap udp port 9000
make pcap-quic    # sudo tcpdump -i lo -w quic_pubsub.pcap udp port 9000
```


## Entornos verificados

- gcc 16.2.1 y clang 23.1.1 en Linux (CachyOS, x86-64).
- OpenSSL 3.6.5. Hace falta 3.5 como mínimo, la primera versión con servidor QUIC.

No se han probado en otros sistemas. `MSG_NOSIGNAL`, `MSG_TRUNC` como valor de retorno y
`SOCK_NONBLOCK` son de Linux.


## Extra: `neko.c` y `bono-neko.c`

Aparte de la entrega hay dos archivos que hacen lo mismo en un solo fuente cada uno,
la mala monitor si no lo revisa :P: `neko.c` (broker, publicador y suscriptor sobre TCP y
UDP) y `bono-neko.c` (Por errores en la implementacion no lo pongo en el repo xd).
```sh
./neko b t 9000                       # rol (b, s, p) y protocolo (t, u)
./neko s t 127.0.0.1 9000 A-B C-D
./neko p t 127.0.0.1 9000 A-B 10
./bono-neko b 9000
```
