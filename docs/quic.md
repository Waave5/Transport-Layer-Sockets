# Publicación–suscripción sobre QUIC con OpenSSL

Este documento cubre lo que pide el enunciado cuando se usa una librería:

> No está permitido el uso de librerías de implementación de sockets a menos que se
> especifique punto a punto la interacción del programa con cada función de la librería,
> debe estar totalmente documentado.

Los seis programas de TCP y UDP (`broker_tcp.c`, `publisher_udp.c`…) no usan ninguna
librería: solo libc y las llamadas de sockets del sistema. Los tres de QUIC
(`broker_quic.c`, `publisher_quic.c`, `subscriber_quic.c`) usan **una**: OpenSSL. Aquí
está cada función de OpenSSL que llaman, con qué argumentos, para qué y qué se hace con
el resultado.

`bono-neko.c` es una versión anterior de lo mismo, con los tres roles en un solo
archivo maquetado; usa las mismas funciones. `docs/ofuscacion.md` lo explica.

---

## 1. Por qué hace falta una librería

Con TCP y UDP el protocolo de transporte vive en el núcleo del sistema operativo: el
programa pide un socket y el núcleo hace el resto. QUIC no está en el núcleo. Es un
protocolo que se implementa en el propio programa, encima de un socket UDP normal, y
que además exige cifrado TLS 1.3 desde el primer paquete (RFC 9000 y RFC 9001).

Implementarlo a mano significa escribir TLS 1.3, criptografía incluida. Por eso se usa
una librería que ya lo trae.

| | |
|---|---|
| Librería | OpenSSL |
| Versión mínima | 3.5 (la primera con servidor QUIC; el cliente existe desde la 3.2) |
| Versión probada | 3.6.5 |
| Cabeceras | `<openssl/ssl.h>`, `<openssl/err.h>` |
| Enlazado | `-lssl -lcrypto` |
| Licencia | Apache 2.0 |

En casi cualquier Linux ya está instalada. Para comprobar la versión: `openssl version`.

### Qué hace la librería y qué hace el programa

| Lo hace OpenSSL | Lo hace el programa |
|---|---|
| Handshake QUIC y TLS 1.3, cifrado de todos los paquetes | Crear el socket UDP y dárselo a OpenSSL |
| Números de paquete, ACK y retransmisiones | Decidir cuándo dormir y cuándo atender (`poll`) |
| Control de flujo y de congestión | El protocolo de aplicación (`S tema`, `P tema seq texto`) |
| Streams: entrega en orden y sin duplicados | La tabla de clientes y de temas del broker |
| PING para mantener viva la conexión, tiempo de inactividad | Reparto de cada publicación a sus suscriptores |

Los programas siguen llamando directamente a las funciones de sockets del sistema, igual
que los de UDP: `socket()` para crear el socket UDP, `setsockopt()` y `bind()` en el
broker, `poll()` para esperar a que llegue un datagrama y `close()` al terminar. El
retorno de todas se comprueba. Todo lo demás pasa por OpenSSL.

---

## 2. Los cuatro tipos de objeto

OpenSSL representa todo con dos tipos, `SSL_CTX` y `SSL`. El mismo tipo `SSL` sirve para
tres cosas distintas según cómo se haya creado:

```
SSL_CTX  ctx        configuración común: QUIC cliente o servidor, certificado, ALPN
   │
   ├─ SSL  escucha      (broker)  "listener": dueño del socket UDP, reparte los datagramas
   │    ├─ SSL  con[i]        una conexión QUIC con un cliente
   │    │    └─ SSL  str[i]   el stream de esa conexión
   │    └─ ...
   │
   └─ SSL  q        (cliente) la conexión con el broker; también es su stream por defecto
```

- En el **broker** hay un listener, y por cada cliente dos objetos: la conexión
  (`con[i]`) y el stream que abre el cliente (`str[i]`). Se lee y se escribe en el
  stream; a la conexión solo se le pregunta si sigue viva.
- En el **cliente** hay un único objeto `q`. OpenSSL crea el stream 0 la primera vez que
  se escribe en `q` y a partir de ahí leer y escribir en `q` es leer y escribir en ese
  stream ("stream por defecto").

Un stream QUIC es, como una conexión TCP, un flujo de bytes fiable y ordenado. Por eso el
protocolo de aplicación es el mismo que en la versión TCP: líneas terminadas en `\n`, que
el broker reensambla porque un mensaje puede llegar partido.

---

## 3. Cada función de OpenSSL, una por una

La columna "Dónde" indica la función que la llama. `main` es siempre el de
`broker_quic.c`. `conectar` está, idéntica, en `publisher_quic.c` y `subscriber_quic.c`;
`publicar` y `publicador` son del primero y `suscriptor` y `dormir` del segundo (el
broker tiene su propio `dormir`).

### 3.1 Comunes a broker y cliente

| Función | Dónde | Argumentos que se le pasan | Qué hace y qué se hace con el resultado |
|---|---|---|---|
| `OSSL_QUIC_server_method()` | `main` | ninguno | Devuelve el descriptor "servidor QUIC". Solo se pasa a `SSL_CTX_new`. |
| `OSSL_QUIC_client_method()` | `conectar` | ninguno | Lo mismo para "cliente QUIC". |
| `SSL_CTX_new(método)` | `main`, `conectar` | uno de los dos anteriores, según el rol | Crea el contexto: el objeto de configuración del que salen todas las conexiones. Si devuelve `NULL`, el programa termina con el motivo que da OpenSSL. |
| `SSL_CTX_set_keylog_callback(ctx, guardar_clave)` | `main`, `conectar` | el contexto y la función `guardar_clave` | Registra una función a la que OpenSSL llama con cada clave de sesión que genera. `guardar_clave` las añade al archivo de `SSLKEYLOGFILE` si esa variable existe. Sirve para descifrar capturas; no cambia el protocolo. |
| `SSL_set_fd(ssl, fd)` | `main`, `conectar` | el listener (broker) o la conexión (cliente), y el socket UDP | Le entrega el socket a OpenSSL. Desde aquí es OpenSSL quien envía y recibe datagramas por él. Devuelve 0 si falla. |
| `SSL_set_blocking_mode(ssl, 0 o 1)` | `main`, `suscriptor` | objeto y modo | Con 0, las operaciones sobre ese objeto no esperan: si no pueden completarse devuelven error "inténtalo luego". El broker lo usa siempre (atiende a muchos clientes en un hilo). El subscriber lo pone a 0 mientras espera mensajes, para poder reaccionar a Ctrl-C, y lo devuelve a 1 para el cierre. |
| `SSL_write_ex(ssl, buf, n, &escritos)` | `procesar_linea`, `publicar`, `suscriptor` | stream, datos, longitud | Encola `n` bytes en el stream; QUIC los envía, espera su ACK y los retransmite si hace falta. Devuelve 1 si se aceptaron todos. En el cliente (modo bloqueante) espera lo necesario; si aun así devuelve 0, la conexión se perdió y el cliente termina con error. En el broker, si devuelve 0, se mira `SSL_get_error`. |
| `SSL_read_ex(ssl, buf, n, &leídos)` | `atender`, `publicador`, `suscriptor` | stream, buffer, tamaño | Copia al buffer los bytes que ya llegaron, en orden. Devuelve 1 y cuántos fueron, o 0 si no hay nada, si el otro extremo cerró o si hubo error; para saber cuál, `SSL_get_error`. |
| `SSL_get_error(ssl, 0)` | `procesar_linea`, `atender`, `suscriptor` | el objeto de la operación que falló | Dice por qué falló la última operación. Los valores que se distinguen están en la tabla 3.4. |
| `SSL_get_event_timeout(ssl, &tv, &infinito)` | `dormir` | listener o conexión | Dice cuánto falta para que QUIC tenga que hacer algo por su cuenta (retransmitir, enviar un PING, cerrar por inactividad). `dormir` espera en `poll()` ese tiempo como máximo, con un tope de 1 s. |
| `SSL_stream_conclude(ssl, 0)` | `atender`, `publicador`, `suscriptor` | stream | Cierra la mitad de envío del stream: QUIC manda el bit FIN detrás del último byte. Todavía se puede seguir leyendo. |
| `SSL_free(ssl)` | `soltar`, `main`, `conectar`, `publicador`, `suscriptor` | stream, conexión o listener | Libera el objeto (acepta `NULL`). En el broker se libera primero el stream y después su conexión; al detenerse, también el listener. |
| `SSL_CTX_free(ctx)` | `main`, `conectar`, `publicador`, `suscriptor` | contexto | Libera el contexto al terminar, o si la conexión no llegó a establecerse. |
| `SSL_set_feature_request_uint(con, SSL_VALUE_QUIC_IDLE_TIMEOUT, 30000)` | `main`, `conectar` | conexión, qué parámetro y su valor en milisegundos | Pide que la conexión se dé por muerta tras 30 s sin noticias del otro extremo. Es una macro sobre `SSL_set_value_uint`. Rige el menor de los valores que piden los dos extremos. Si devuelve 0 se sigue con el valor por defecto de la librería, que también es 30 s. |
| `SSL_shutdown(con)` | `publicador`, `suscriptor`, `main` | conexión | Cierra la conexión: envía `CONNECTION_CLOSE`. Devuelve 0 mientras está en curso y 1 al terminar. Los clientes la repiten hasta que deja de devolver 0. El broker, al detenerse, la llama una vez por cliente sin esperar. |
| `ERR_print_errors_fp(stderr)` | `morir_ssl`, `error_ssl` | `stderr` | Vuelca el motivo del último fallo según OpenSSL (por ejemplo, "certificate verify failed") y vacía su cola de errores. Solo se usa antes de abandonar. |
| `ERR_clear_error()` | `main`, `atender`, `conectar`, `publicador`, `suscriptor` | ninguno | Vacía la cola de errores de OpenSSL. Se llama tras un fallo esperado ("ahora no" en modo no bloqueante, un cierre que ya estaba hecho) para que no se acumule ni se confunda con un error posterior. |

### 3.2 Solo en el broker

| Función | Dónde | Argumentos | Qué hace y qué se hace con el resultado |
|---|---|---|---|
| `SSL_CTX_use_certificate_chain_file(ctx, "bono-cert.pem")` | `main` | contexto y ruta | Carga el certificado que el broker presentará en el handshake. Si no devuelve 1, el broker termina avisando de que faltan los `.pem`. |
| `SSL_CTX_use_PrivateKey_file(ctx, "bono-key.pem", SSL_FILETYPE_PEM)` | `main` | contexto, ruta y formato | Carga la clave privada que corresponde a ese certificado. Mismo tratamiento del error. |
| `SSL_CTX_set_alpn_select_cb(ctx, elegir_alpn, NULL)` | `main` | contexto y la función `elegir_alpn` | Registra la función que elige el protocolo de aplicación. QUIC obliga a negociar uno (ALPN); sin esto el handshake falla. |
| `SSL_select_next_proto(&out, &len, "\4neko", 5, oferta, n)` | `elegir_alpn` | la lista del servidor y la que ofrece el cliente | Busca un protocolo común entre las dos listas. Si lo encuentra (`OPENSSL_NPN_NEGOTIATED`), `elegir_alpn` devuelve OK; si no, devuelve error fatal y OpenSSL rechaza la conexión. |
| `SSL_new_listener(ctx, 0)` | `main` | contexto | Crea el listener: el objeto que recibe los datagramas de todos los clientes y sabe a qué conexión pertenece cada uno. Es el equivalente QUIC del socket de escucha de TCP. |
| `SSL_listen(escucha)` | `main` | listener | Empieza a aceptar conexiones entrantes. A partir de aquí el listener contesta al primer `Initial` de cada cliente con un `Retry`, para comprobar que la dirección de origen es real. |
| `SSL_handle_events(escucha)` | `main` | listener | Hace avanzar a QUIC: lee los datagramas pendientes del socket, los descifra, actualiza todas las conexiones y envía lo que haya que enviar (ACK, retransmisiones, datos encolados). Se llama una vez al principio de cada vuelta del bucle y una última vez al detenerse, para que salgan los avisos de cierre. |
| `SSL_accept_connection(escucha, SSL_ACCEPT_CONNECTION_NO_BLOCK)` | `main` | listener, sin esperar | Devuelve una conexión nueva ya establecida, o `NULL` si no hay ninguna. El broker la guarda en la primera entrada libre de `con[]`, o la libera con `SSL_free` si la tabla está llena. |
| `SSL_accept_stream(con[i], SSL_ACCEPT_STREAM_NO_BLOCK)` | `main` | conexión, sin esperar | Devuelve el stream que abrió el cliente, o `NULL` si todavía no ha abierto ninguno. Se guarda en `str[i]`. |
| `SSL_get_conn_close_info(con[i], &info, sizeof info)` | `main`, `atender` | conexión y una estructura donde dejar el motivo | Devuelve 1 si la conexión ya terminó (la cerró el cliente, hubo un error o venció el tiempo de inactividad). Solo se usa el valor devuelto: si es 1, `soltar(i)` libera la entrada. |

### 3.3 Solo en el cliente (publisher y subscriber)

| Función | Dónde | Argumentos | Qué hace y qué se hace con el resultado |
|---|---|---|---|
| `SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, 0)` | `conectar` | contexto y modo | Exige que el broker presente un certificado válido; si no, el handshake falla. |
| `SSL_CTX_load_verify_file(ctx, "bono-cert.pem")` | `conectar` | contexto y ruta | Indica en qué certificado se confía. Como el del broker es autofirmado, el cliente se fía exactamente de ese archivo. Si no existe, el cliente termina. |
| `SSL_new(ctx)` | `conectar` | contexto | Crea el objeto de la conexión con el broker. |
| `BIO_ADDR_new()` | `conectar` | ninguno | Reserva una estructura de dirección de OpenSSL. |
| `BIO_ADDR_rawmake(peer, AF_INET, &ip, 4, puerto)` | `conectar` | la estructura, familia, los 4 bytes de la IP y el puerto en orden de red | Rellena la estructura con la IP y el puerto del broker. |
| `SSL_set1_initial_peer_addr(q, peer)` | `conectar` | conexión y dirección | Le dice a OpenSSL a dónde enviar el primer paquete. UDP no tiene `connect`, así que la dirección se da aquí. |
| `BIO_ADDR_free(peer)` | `conectar` | la estructura | La libera; OpenSSL ya guardó una copia. |
| `SSL_set_alpn_protos(q, "\4neko", 5)` | `conectar` | conexión y lista de protocolos | Anuncia el protocolo de aplicación que habla el cliente. Al revés que casi todas: devuelve 0 si todo va bien. |
| `SSL_connect(q)` | `conectar` | conexión | Hace el handshake completo de QUIC y TLS 1.3 y no vuelve hasta que termina. Si no devuelve 1 (broker apagado, certificado que no coincide, ALPN distinto), el cliente termina con el motivo. |
| `SSL_get_version(q)` | `conectar` | conexión | Devuelve el texto `"QUICv1"`. Solo se imprime. |
| `SSL_get_cipher_name(q)` | `conectar` | conexión | Devuelve el nombre del cifrado negociado, por ejemplo `TLS_AES_256_GCM_SHA384`. Solo se imprime. |

### 3.4 Constantes

Los tres programas usan las constantes por su nombre. En `bono-neko.c` están escritas
con su valor numérico; esta es la equivalencia:

| Constante | Valor | Significado |
|---|---|---|
| `SSL_ERROR_SSL` | 1 | Error del protocolo; en QUIC, también "la conexión ya no existe". |
| `SSL_ERROR_WANT_READ` | 2 | No hay datos todavía; reintentar cuando llegue algo. |
| `SSL_ERROR_WANT_WRITE` | 3 | No cabe más en el stream (control de flujo); reintentar más tarde con los mismos datos. |
| `SSL_ERROR_ZERO_RETURN` | 6 | El otro extremo cerró su mitad del stream y ya se leyó todo. |
| `SSL_VERIFY_PEER` | 1 | Comprobar el certificado del otro extremo. |
| `SSL_FILETYPE_PEM` | 1 | El archivo está en formato PEM. |
| `SSL_ACCEPT_CONNECTION_NO_BLOCK` | 1 | No esperar si no hay conexión nueva. |
| `SSL_ACCEPT_STREAM_NO_BLOCK` | 1 | No esperar si no hay stream nuevo. |
| `OPENSSL_NPN_NEGOTIATED` | 1 | Se encontró un protocolo ALPN común. |
| `SSL_TLSEXT_ERR_OK` / `SSL_TLSEXT_ERR_ALERT_FATAL` | 0 / 2 | Lo que devuelve la función `elegir_alpn`. |
| `SSL_VALUE_QUIC_IDLE_TIMEOUT` | 5 | El parámetro "tiempo de inactividad" de una conexión. |
| `SOCK_DGRAM \| SOCK_NONBLOCK` | 2050 | Socket UDP no bloqueante (esto es del sistema, no de OpenSSL). |

`"\4neko"` es la lista ALPN en el formato que exige TLS: un byte con la longitud (4) y
después el nombre.

---

## 4. Paso a paso

### 4.1 Arranque del broker

1. `socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0)`, `setsockopt(SO_RCVBUF)` y `bind()`:
   un socket UDP en el puerto pedido, con un buffer de recepción de 1 MiB porque todas
   las conexiones comparten ese socket. Lo crea el programa, no OpenSSL.
2. `SSL_CTX_new(OSSL_QUIC_server_method())`: contexto de servidor QUIC.
3. `SSL_CTX_use_certificate_chain_file` y `SSL_CTX_use_PrivateKey_file`: identidad del
   broker.
4. `SSL_CTX_set_alpn_select_cb`: solo se aceptan clientes que hablen `neko`.
5. `SSL_new_listener`, `SSL_set_fd`, `SSL_listen`: el listener se queda con el socket.
6. `SSL_set_blocking_mode(escucha, 0)`: nada de lo que venga después espera.

### 4.2 El bucle del broker

Cada vuelta hace cuatro cosas:

1. `SSL_handle_events(escucha)`: QUIC procesa lo que haya llegado y envía lo pendiente.
2. Mientras `SSL_accept_connection` devuelva una conexión, se le asigna una entrada y se
   anota `[+] cliente N conectado`.
3. Para cada cliente: si aún no tiene stream, `SSL_accept_stream`; si lo tiene,
   `atender(i)`:
   - Despacha las líneas completas que haya en su buffer. Una `S tema` apunta la
     suscripción. Una `P tema …` se escribe con `SSL_write_ex` en el stream de cada
     suscriptor de ese tema.
   - Lee más con `SSL_read_ex` y repite, hasta que `SSL_get_error` diga
     `SSL_ERROR_WANT_READ` (no hay más por ahora).
4. Si en toda la vuelta no se hizo nada, `dormir(escucha)`: `poll()` sobre el socket hasta que
   llegue un datagrama o venza el plazo de `SSL_get_event_timeout`.

**Contrapresión.** Si `SSL_write_ex` hacia un suscriptor falla con `SSL_ERROR_WANT_WRITE`,
su stream está lleno: QUIC no deja enviar más hasta que ese suscriptor confirme lo
anterior. El broker no descarta el mensaje. Apunta por qué suscriptor iba (`desde[i]`),
deja la línea en el buffer del publisher y **deja de leer de ese publisher**. Al no leer,
el control de flujo de QUIC acaba frenando al publisher en origen. En la siguiente vuelta
reintenta la misma escritura con los mismos bytes, que es lo que exige OpenSSL para
continuar una escritura a medias. `dueno[j]` evita que otro publisher escriba en ese stream
mientras tanto y se mezclen dos mensajes.

### 4.3 Conexión de un cliente

1. `socket()`: un socket UDP no bloqueante.
2. `SSL_CTX_new(OSSL_QUIC_client_method())`.
3. `SSL_CTX_set_verify` y `SSL_CTX_load_verify_file`: solo vale el broker que tenga la
   clave de `bono-cert.pem`.
4. `SSL_new`, `SSL_set_fd`.
5. `BIO_ADDR_new`, `BIO_ADDR_rawmake`, `SSL_set1_initial_peer_addr`: dirección del broker.
6. `SSL_set_alpn_protos`: "hablo `neko`".
7. `SSL_connect`: el handshake. En la captura son los paquetes `Initial`, `Retry` y
   `Handshake`.
8. Se imprime `[quic] conectado: QUICv1, <cifrado>` con `SSL_get_version` y
   `SSL_get_cipher_name`.

### 4.4 Publicar y suscribirse

- **Publisher**: un `SSL_write_ex(q, "P tema seq texto\n")` por mensaje. El primero crea
  el stream 0. En la captura son tramas `STREAM(0)` del cliente al broker, cada una
  confirmada con un `ACK`.
- **Subscriber**: un `SSL_write_ex(q, "S tema\n")` por tema. Después pasa a modo no
  bloqueante y repite `SSL_read_ex`: si hay datos los copia a la salida; si
  `SSL_get_error` dice `SSL_ERROR_WANT_READ`, espera en `dormir(q)`; cualquier otra cosa es
  que el broker cerró. Un stream entrega bytes, no mensajes, así que el subscriber
  junta líneas completas antes de mostrarlas. Ctrl-C interrumpe el `poll()` de `dormir` y sale del bucle.

### 4.5 Cierre

El cierre es en tres pasos para que no se pierda nada de lo último que se envió:

1. El cliente llama a `SSL_stream_conclude(q, 0)`: "no voy a enviar más". Viaja como el
   bit FIN de la última trama `STREAM`.
2. El broker sigue leyendo hasta que `SSL_get_error` devuelve `SSL_ERROR_ZERO_RETURN`:
   ya leyó todo. Entonces borra los temas del cliente y llama él a `SSL_stream_conclude`.
3. El cliente, que estaba en `SSL_read_ex`, ve ese FIN (la lectura devuelve 0) y llama a
   `SSL_shutdown`, que envía `CONNECTION_CLOSE`. El broker lo detecta con
   `SSL_get_conn_close_info`, anota `[-] cliente N desconectado` y libera stream y
   conexión con `SSL_free`.

Si un cliente desaparece sin cerrar, no llega ningún FIN. QUIC lo da por perdido cuando
pasan 30 s sin recibir nada de él, `SSL_get_conn_close_info` pasa a devolver 1 y el
broker lo libera igual.

Si es el broker el que se detiene (Ctrl-C), llama a `SSL_shutdown` sobre cada conexión
y a `SSL_handle_events` para que salgan los `CONNECTION_CLOSE`, y libera todo. Los
subscribers lo notan en menos de un segundo y terminan con "el broker cerro la conexion".

---

## 5. El certificado

QUIC no tiene modo sin cifrar. El broker necesita un certificado y su clave privada, y
los crea el `Makefile` la primera vez:

```sh
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 365 \
    -subj "/CN=neko-broker" -keyout bono-key.pem -out bono-cert.pem
```

| Archivo | Quién lo usa | Para qué |
|---|---|---|
| `bono-key.pem` | solo el broker | Clave privada. Demuestra en el handshake que es el dueño del certificado. |
| `bono-cert.pem` | broker y clientes | El broker lo presenta; los clientes lo comparan con el que reciben. |

Es autofirmado: no lo avala ninguna autoridad. Los clientes se fían de él porque tienen
una copia del mismo archivo. Si el broker presenta otro, `SSL_connect` falla con
`certificate verify failed`. No se comprueba el nombre ni la IP del certificado, solo
que sea ese. Para usar el programa entre dos máquinas hay que copiar `bono-cert.pem` a
la máquina del cliente.

---

## 6. Ver QUIC en Wireshark

`capturas/quic_pubsub.pcap` es el escenario del enunciado (1 broker, 2 subscribers,
2 publishers de 10 mensajes) y `capturas/quic_claves.log` son sus claves de sesión.

**Sin las claves** Wireshark ya lo reconoce como QUIC (filtro `quic`) y muestra el
establecimiento, que va en claro o con claves derivables:

```
43055 → 9000   Initial, DCID=a256da90aa526f55, PKN: 0, CRYPTO
9000 → 43055   Retry, SCID=ba05795fc8b28a40
43055 → 9000   Initial, DCID=ba05795fc8b28a40, PKN: 2, CRYPTO
9000 → 43055   Initial, SCID=ba05795fc8b28a40, PKN: 1, ACK, CRYPTO
9000 → 43055   Handshake, SCID=ba05795fc8b28a40, PKN: 0, CRYPTO
43055 → 9000   Handshake, DCID=ba05795fc8b28a40, PKN: 0, ACK, CRYPTO
```

Lo que viene después aparece como `Protected Payload`, sin más detalle.

**Con las claves** se ve el contenido. En Wireshark: Editar → Preferencias → Protocols →
TLS → "(Pre)-Master-Secret log filename" → elegir `quic_claves.log`. O con `tshark`:

```sh
tshark -r capturas/quic_pubsub.pcap -o tls.keylog_file:capturas/quic_claves.log -Y quic
```

```
43055 → 9000   Protected Payload (KP0), PKN: 0, STREAM(0)        el "S A-B" del suscriptor
9000 → 43055   Protected Payload (KP0), PKN: 0, DONE, ACK, NT, CRYPTO
9000 → 43055   Protected Payload (KP0), PKN: 1, STREAM(0)        un mensaje reenviado
43055 → 9000   Protected Payload (KP0), PKN: 2, ACK
```

Correspondencia con lo que hace el programa:

| En la captura | Lo provoca |
|---|---|
| `Initial` con `CRYPTO` | `SSL_connect` (ClientHello de TLS dentro de QUIC) |
| `Retry` | `SSL_listen`: el listener valida la dirección del cliente antes de gastar recursos |
| `Handshake` con `CRYPTO` | El certificado del broker y el final del handshake TLS |
| `DONE` (HANDSHAKE_DONE) | El broker confirma que el handshake terminó |
| `NT` (NEW_TOKEN) | El broker da al cliente un testigo para futuras conexiones |
| `STREAM(0)` | Cada `SSL_write_ex` |
| `ACK` | Lo genera OpenSSL solo; el programa no interviene |
| `PING` | OpenSSL mantiene viva una conexión sin tráfico |
| `STREAM(0)` con FIN | `SSL_stream_conclude` |
| `CONNECTION_CLOSE` | `SSL_shutdown` |

Diferencias con TCP que se aprecian en la captura:

- No hay SYN, SYN-ACK ni FIN: no existen esas banderas. El establecimiento son los
  paquetes `Initial` y `Handshake`, que además negocian el cifrado en el mismo viaje.
- Los ACK existen pero son tramas dentro del paquete cifrado, no un campo de la cabecera.
- La conexión no se identifica por IP y puerto sino por el identificador de conexión
  (`DCID` / `SCID`).
- Los primeros paquetes van rellenos hasta 1200 bytes (`PADDING`): QUIC lo exige para
  que no se pueda usar un servidor como amplificador de tráfico.

Para tomar una captura propia hay que lanzar los tres programas con la misma variable:

```sh
SSLKEYLOGFILE=claves.log ./broker_quic 9000
SSLKEYLOGFILE=claves.log ./subscriber_quic 127.0.0.1 9000 A-B
SSLKEYLOGFILE=claves.log ./publisher_quic 127.0.0.1 9000 A-B 10
```

---


## 7. Qué se probó

`./try.sh` lanza 1 broker, 2 subscribers y 2 publishers de 10 mensajes y comprueba que
cada subscriber recibe exactamente lo suyo y en orden, que el broker registró las 23
publicaciones y que los procesos que se detienen con Ctrl-C salen con código 0. Aparte
se probó a mano:

| Prueba | Resultado |
|---|---|
| 5 publishers de 2000 líneas a 2 subscribers | 10 000 de 10 000 y 2000 de 2000, en orden, sin duplicados |
| Lo mismo con AddressSanitizer y UBSan | Sin errores de memoria ni fugas |
| 50 mensajes con un 20 % de pérdida de paquetes simulada | 50 de 50, en orden (UDP entregó 34) |
| Modo interactivo con 2 subscribers y 2 publishers | Cada proceso abre su conexión; Ctrl-C las cierra todas |
| Broker detenido con un subscriber conectado | El subscriber termina en menos de 1 s |
| Broker apagado al conectar | El cliente termina con error a los 30 s |
| Cliente con un `bono-cert.pem` distinto | `certificate verify failed`; no se conecta |
| Captura con y sin claves | Wireshark la identifica como QUIC y, con claves, muestra `STREAM` y `ACK` |

Con `bono-neko`, que usa las mismas llamadas, se probaron además 300 conexiones
seguidas, un subscriber muerto con `kill -9` y un subscriber 75 s sin tráfico, que
sigue conectado.

---

## 8. Dónde está cada cosa que pide el enunciado

| Punto del enunciado | Dónde se cumple |
|---|---|
| 5.1 Todo en C, compilado con gcc en Linux | Los nueve `.c` de la raíz; `make all` |
| 5.1 `broker_tcp.c`, `publisher_tcp.c`, `subscriber_tcp.c`, `broker_udp.c`, `publisher_udp.c`, `subscriber_udp.c` | Están en la raíz, cada uno con su `main` |
| 5.2 Al menos un broker, dos suscriptores y dos publicadores a la vez | `./try.sh`; el modo interactivo; las capturas de `capturas/` |
| 6.1 Sistema publicador–suscriptor en TCP y en UDP | Los seis archivos anteriores |
| 6.2 Librerías de sockets solo si se documenta cada función | TCP y UDP no usan ninguna; para QUIC, este documento |
| 6.3 Al menos 10 mensajes por publicador, verificados | `./try.sh` |
| 6.4 Capturas `tcp_pubsub.pcap` y `udp_pubsub.pcap` | `capturas/` (más `quic_pubsub.pcap`) |
| 6.5 y 6.6 Tabla comparativa y preguntas de análisis | Van en el informe; no están en este repositorio |
| 7.2 Fuentes organizados y comentados | Los nueve archivos, comentados función por función |
| 7.2 Documentación en un README si hay librerías externas | `README.md`, que remite aquí |
| Bono: lo mismo con QUIC | `broker_quic.c`, `publisher_quic.c`, `subscriber_quic.c` |
