# Experimento: el escenario del enunciado en TCP, UDP y QUIC

Para cada protocolo se lanzaron a mano cinco procesos, cada uno con su salida guardada
por separado: 1 broker, 2 suscriptores y 2 publicadores de 10 mensajes. Los tres
experimentos corrieron a la vez en la misma máquina (loopback), cada uno en su puerto.
Todo lo que aparece en los bloques es la salida literal de los programas.

## Secuencia

| Paso | Qué se lanza | Comando (con `tcp`; igual con `udp` y `quic`) |
|---|---|---|
| 1 | Broker | `./broker_tcp PUERTO` |
| 2 | Suscriptor 1, sigue los dos partidos | `./subscriber_tcp 127.0.0.1 PUERTO A-B C-D` |
| 3 | Suscriptor 2, sigue solo C-D | `./subscriber_tcp 127.0.0.1 PUERTO C-D` |
| 4 | Publicador 1, partido A-B | `./publisher_tcp 127.0.0.1 PUERTO A-B 10` |
| 5 | Publicador 2, partido C-D | `./publisher_tcp 127.0.0.1 PUERTO C-D 10` |
| 6 | Se espera a que terminen los dos publicadores | |
| 7 | Ctrl-C (SIGINT) a los dos suscriptores | |
| 8 | Ctrl-C (SIGINT) al broker | |

Entre el paso 1 y el 2 se espera 0,5 s, y entre el 3 y el 4, 1 s, para que el broker
esté escuchando y las suscripciones estén hechas antes de publicar. Los pasos 2 y 3 se
lanzan seguidos, igual que el 4 y el 5.

## Resumen

| | TCP | UDP | QUIC |
|---|---|---|---|
| Puerto | 9101 | 9102 | 9103 |
| Mensajes que recibió el suscriptor 1 (esperados 20) | 20 | 20 | 20 |
| Mensajes que recibió el suscriptor 2 (esperados 10) | 10 | 10 | 10 |
| Suscriptor 1, secuencia de A-B | 1 a 10, en orden | 1 a 10, en orden | 1 a 10, en orden |
| Suscriptor 1, secuencia de C-D | 1 a 10, en orden | 1 a 10, en orden | 1 a 10, en orden |
| Suscriptor 2, secuencia de C-D | 1 a 10, en orden | 1 a 10, en orden | 1 a 10, en orden |
| Publicaciones registradas por el broker (esperadas 20) | 20 | 20 | 20 |
| Conexiones abiertas `[+]` | 4 | 0 | 4 |
| Bajas o cierres `[-]` | 4 | 2 | 4 |
| Tiempo desde que arrancan los publicadores hasta que terminan (s) | 0,214 | 0,236 | 0,461 |
| Código de salida de los cinco procesos | todos 0 | todos 0 | todos 0 |

Los tres entregaron los 30 mensajes esperados, completos y en orden, y los cinco
procesos de cada experimento terminaron limpiamente. En loopback no hay pérdida, así
que UDP también entrega todo; la diferencia entre protocolos está en el registro del
broker (abajo) y en las capturas de `capturas/`.

Cada experimento se ejecutó dos veces (la primera pasada no midió el tiempo porque la
máquina no tiene `bc`). Las salidas que se muestran son las de la segunda; la primera
dio los mismos conteos, secuencias y códigos de salida.

## Qué mirar en cada registro

- **TCP**: hay 4 líneas `[+]`, una por conexión aceptada, y 4 `[-]`, una por cierre. Los
  publicadores (clientes 3 y 4) se desconectan solos al terminar; los suscriptores
  (clientes 1 y 2), con el Ctrl-C.
- **UDP**: no hay ninguna línea `[+]`. El broker no sabe que un publicador existe hasta
  que le llega un datagrama, ni que terminó. Los suscriptores aparecen identificados
  por IP y puerto, y solo hay `[-]` porque al salir envían el mensaje `U`.
- **QUIC**: vuelve a haber 4 `[+]` y 4 `[-]`, como en TCP, y cada cliente anuncia la
  conexión cifrada (`QUICv1, TLS_AES_256_GCM_SHA384`). Tardó el doble que los otros
  dos. La causa no se midió; lo esperable es que sea el handshake con TLS 1.3 que hace
  cada publicador antes de enviar y el cierre en tres pasos al terminar.

Dos detalles que cambian de una ejecución a otra y no son errores:

- **Numeración de clientes**: los dos suscriptores se lanzan a la vez, así que el broker
  numera primero al que le llega antes. En TCP y en QUIC fue el suscriptor 2.
- **Intercalado A-B / C-D**: los dos publicadores también arrancan a la vez. En esta
  pasada salió primero A-B; en la anterior, C-D. Dentro de cada partido el orden nunca
  cambia.

## TCP (puerto 9101)

### Broker

```
[*] broker TCP en el puerto 9101 (Ctrl-C para salir)
[+] cliente 1 conectado
[S] cliente 1 se suscribe a C-D
[+] cliente 2 conectado
[S] cliente 2 se suscribe a A-B
[S] cliente 2 se suscribe a C-D
[+] cliente 3 conectado
[P] A-B 1 Gol de Equipo A al minuto 8 -> 1 suscriptores
[+] cliente 4 conectado
[P] C-D 1 Gol de Equipo A al minuto 8 -> 2 suscriptores
[P] A-B 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15 -> 1 suscriptores
[P] C-D 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15 -> 2 suscriptores
[P] A-B 3 Cambio: jugador 10 entra por jugador 20 al minuto 22 -> 1 suscriptores
[P] C-D 3 Cambio: jugador 10 entra por jugador 20 al minuto 22 -> 2 suscriptores
[P] A-B 4 Tiro de esquina para Equipo B al minuto 29 -> 1 suscriptores
[P] C-D 4 Tiro de esquina para Equipo B al minuto 29 -> 2 suscriptores
[P] A-B 5 Fuera de lugar de Equipo A al minuto 36 -> 1 suscriptores
[P] C-D 5 Fuera de lugar de Equipo A al minuto 36 -> 2 suscriptores
[P] A-B 6 Gol de Equipo B al minuto 43 -> 1 suscriptores
[P] C-D 6 Gol de Equipo B al minuto 43 -> 2 suscriptores
[P] A-B 7 Tarjeta roja al numero 4 de Equipo A al minuto 50 -> 1 suscriptores
[P] C-D 7 Tarjeta roja al numero 4 de Equipo A al minuto 50 -> 2 suscriptores
[P] A-B 8 Atajada del portero de Equipo B al minuto 57 -> 1 suscriptores
[P] C-D 8 Atajada del portero de Equipo B al minuto 57 -> 2 suscriptores
[P] A-B 9 Gol de Equipo A al minuto 64 -> 1 suscriptores
[P] C-D 9 Gol de Equipo A al minuto 64 -> 2 suscriptores
[P] A-B 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71 -> 1 suscriptores
[P] C-D 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71 -> 2 suscriptores
[-] cliente 3 desconectado (cerro la conexion)
[-] cliente 4 desconectado (cerro la conexion)
[-] cliente 1 desconectado (cerro la conexion)
[-] cliente 2 desconectado (cerro la conexion)
[*] broker detenido
```

### Suscriptor 1 (A-B y C-D)

```
A-B 1 Gol de Equipo A al minuto 8
C-D 1 Gol de Equipo A al minuto 8
A-B 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15
C-D 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15
A-B 3 Cambio: jugador 10 entra por jugador 20 al minuto 22
C-D 3 Cambio: jugador 10 entra por jugador 20 al minuto 22
A-B 4 Tiro de esquina para Equipo B al minuto 29
C-D 4 Tiro de esquina para Equipo B al minuto 29
A-B 5 Fuera de lugar de Equipo A al minuto 36
C-D 5 Fuera de lugar de Equipo A al minuto 36
A-B 6 Gol de Equipo B al minuto 43
C-D 6 Gol de Equipo B al minuto 43
A-B 7 Tarjeta roja al numero 4 de Equipo A al minuto 50
C-D 7 Tarjeta roja al numero 4 de Equipo A al minuto 50
A-B 8 Atajada del portero de Equipo B al minuto 57
C-D 8 Atajada del portero de Equipo B al minuto 57
A-B 9 Gol de Equipo A al minuto 64
C-D 9 Gol de Equipo A al minuto 64
A-B 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71
C-D 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71
```

### Suscriptor 2 (C-D)

```
C-D 1 Gol de Equipo A al minuto 8
C-D 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15
C-D 3 Cambio: jugador 10 entra por jugador 20 al minuto 22
C-D 4 Tiro de esquina para Equipo B al minuto 29
C-D 5 Fuera de lugar de Equipo A al minuto 36
C-D 6 Gol de Equipo B al minuto 43
C-D 7 Tarjeta roja al numero 4 de Equipo A al minuto 50
C-D 8 Atajada del portero de Equipo B al minuto 57
C-D 9 Gol de Equipo A al minuto 64
C-D 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71
```

### Publicador 1 (A-B)

```
enviado: A-B 1 Gol de Equipo A al minuto 8
enviado: A-B 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15
enviado: A-B 3 Cambio: jugador 10 entra por jugador 20 al minuto 22
enviado: A-B 4 Tiro de esquina para Equipo B al minuto 29
enviado: A-B 5 Fuera de lugar de Equipo A al minuto 36
enviado: A-B 6 Gol de Equipo B al minuto 43
enviado: A-B 7 Tarjeta roja al numero 4 de Equipo A al minuto 50
enviado: A-B 8 Atajada del portero de Equipo B al minuto 57
enviado: A-B 9 Gol de Equipo A al minuto 64
enviado: A-B 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71
```

### Publicador 2 (C-D)

```
enviado: C-D 1 Gol de Equipo A al minuto 8
enviado: C-D 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15
enviado: C-D 3 Cambio: jugador 10 entra por jugador 20 al minuto 22
enviado: C-D 4 Tiro de esquina para Equipo B al minuto 29
enviado: C-D 5 Fuera de lugar de Equipo A al minuto 36
enviado: C-D 6 Gol de Equipo B al minuto 43
enviado: C-D 7 Tarjeta roja al numero 4 de Equipo A al minuto 50
enviado: C-D 8 Atajada del portero de Equipo B al minuto 57
enviado: C-D 9 Gol de Equipo A al minuto 64
enviado: C-D 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71
```

## UDP (puerto 9102)

### Broker

```
[*] broker UDP en el puerto 9102, buffer de recepcion de 2097152 bytes (Ctrl-C para salir)
[S] suscriptor 0 (127.0.0.1:33216) se suscribe a A-B
[S] suscriptor 0 (127.0.0.1:33216) se suscribe a C-D
[S] suscriptor 1 (127.0.0.1:57041) se suscribe a C-D
[P] A-B 1 Gol de Equipo A al minuto 8 -> 1 suscriptores
[P] C-D 1 Gol de Equipo A al minuto 8 -> 2 suscriptores
[P] A-B 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15 -> 1 suscriptores
[P] C-D 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15 -> 2 suscriptores
[P] A-B 3 Cambio: jugador 10 entra por jugador 20 al minuto 22 -> 1 suscriptores
[P] C-D 3 Cambio: jugador 10 entra por jugador 20 al minuto 22 -> 2 suscriptores
[P] A-B 4 Tiro de esquina para Equipo B al minuto 29 -> 1 suscriptores
[P] C-D 4 Tiro de esquina para Equipo B al minuto 29 -> 2 suscriptores
[P] A-B 5 Fuera de lugar de Equipo A al minuto 36 -> 1 suscriptores
[P] C-D 5 Fuera de lugar de Equipo A al minuto 36 -> 2 suscriptores
[P] A-B 6 Gol de Equipo B al minuto 43 -> 1 suscriptores
[P] C-D 6 Gol de Equipo B al minuto 43 -> 2 suscriptores
[P] A-B 7 Tarjeta roja al numero 4 de Equipo A al minuto 50 -> 1 suscriptores
[P] C-D 7 Tarjeta roja al numero 4 de Equipo A al minuto 50 -> 2 suscriptores
[P] A-B 8 Atajada del portero de Equipo B al minuto 57 -> 1 suscriptores
[P] C-D 8 Atajada del portero de Equipo B al minuto 57 -> 2 suscriptores
[P] A-B 9 Gol de Equipo A al minuto 64 -> 1 suscriptores
[P] C-D 9 Gol de Equipo A al minuto 64 -> 2 suscriptores
[P] A-B 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71 -> 1 suscriptores
[P] C-D 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71 -> 2 suscriptores
[-] suscriptor 1 (127.0.0.1:57041) se da de baja
[-] suscriptor 0 (127.0.0.1:33216) se da de baja
[*] broker detenido
```

### Suscriptor 1 (A-B y C-D)

```
A-B 1 Gol de Equipo A al minuto 8
C-D 1 Gol de Equipo A al minuto 8
A-B 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15
C-D 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15
A-B 3 Cambio: jugador 10 entra por jugador 20 al minuto 22
C-D 3 Cambio: jugador 10 entra por jugador 20 al minuto 22
A-B 4 Tiro de esquina para Equipo B al minuto 29
C-D 4 Tiro de esquina para Equipo B al minuto 29
A-B 5 Fuera de lugar de Equipo A al minuto 36
C-D 5 Fuera de lugar de Equipo A al minuto 36
A-B 6 Gol de Equipo B al minuto 43
C-D 6 Gol de Equipo B al minuto 43
A-B 7 Tarjeta roja al numero 4 de Equipo A al minuto 50
C-D 7 Tarjeta roja al numero 4 de Equipo A al minuto 50
A-B 8 Atajada del portero de Equipo B al minuto 57
C-D 8 Atajada del portero de Equipo B al minuto 57
A-B 9 Gol de Equipo A al minuto 64
C-D 9 Gol de Equipo A al minuto 64
A-B 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71
C-D 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71
```

### Suscriptor 2 (C-D)

```
C-D 1 Gol de Equipo A al minuto 8
C-D 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15
C-D 3 Cambio: jugador 10 entra por jugador 20 al minuto 22
C-D 4 Tiro de esquina para Equipo B al minuto 29
C-D 5 Fuera de lugar de Equipo A al minuto 36
C-D 6 Gol de Equipo B al minuto 43
C-D 7 Tarjeta roja al numero 4 de Equipo A al minuto 50
C-D 8 Atajada del portero de Equipo B al minuto 57
C-D 9 Gol de Equipo A al minuto 64
C-D 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71
```

### Publicador 1 (A-B)

```
enviado: A-B 1 Gol de Equipo A al minuto 8
enviado: A-B 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15
enviado: A-B 3 Cambio: jugador 10 entra por jugador 20 al minuto 22
enviado: A-B 4 Tiro de esquina para Equipo B al minuto 29
enviado: A-B 5 Fuera de lugar de Equipo A al minuto 36
enviado: A-B 6 Gol de Equipo B al minuto 43
enviado: A-B 7 Tarjeta roja al numero 4 de Equipo A al minuto 50
enviado: A-B 8 Atajada del portero de Equipo B al minuto 57
enviado: A-B 9 Gol de Equipo A al minuto 64
enviado: A-B 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71
```

### Publicador 2 (C-D)

```
enviado: C-D 1 Gol de Equipo A al minuto 8
enviado: C-D 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15
enviado: C-D 3 Cambio: jugador 10 entra por jugador 20 al minuto 22
enviado: C-D 4 Tiro de esquina para Equipo B al minuto 29
enviado: C-D 5 Fuera de lugar de Equipo A al minuto 36
enviado: C-D 6 Gol de Equipo B al minuto 43
enviado: C-D 7 Tarjeta roja al numero 4 de Equipo A al minuto 50
enviado: C-D 8 Atajada del portero de Equipo B al minuto 57
enviado: C-D 9 Gol de Equipo A al minuto 64
enviado: C-D 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71
```

## QUIC (puerto 9103)

### Broker

```
[*] broker QUIC en el puerto 9103 (Ctrl-C para salir)
[+] cliente 0 conectado
[+] cliente 1 conectado
[S] cliente 0 se suscribe a C-D
[S] cliente 1 se suscribe a A-B
[S] cliente 1 se suscribe a C-D
[+] cliente 2 conectado
[+] cliente 3 conectado
[P] A-B 1 Gol de Equipo A al minuto 8 -> 1 suscriptores
[P] C-D 1 Gol de Equipo A al minuto 8 -> 2 suscriptores
[P] A-B 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15 -> 1 suscriptores
[P] C-D 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15 -> 2 suscriptores
[P] A-B 3 Cambio: jugador 10 entra por jugador 20 al minuto 22 -> 1 suscriptores
[P] C-D 3 Cambio: jugador 10 entra por jugador 20 al minuto 22 -> 2 suscriptores
[P] A-B 4 Tiro de esquina para Equipo B al minuto 29 -> 1 suscriptores
[P] C-D 4 Tiro de esquina para Equipo B al minuto 29 -> 2 suscriptores
[P] A-B 5 Fuera de lugar de Equipo A al minuto 36 -> 1 suscriptores
[P] C-D 5 Fuera de lugar de Equipo A al minuto 36 -> 2 suscriptores
[P] A-B 6 Gol de Equipo B al minuto 43 -> 1 suscriptores
[P] C-D 6 Gol de Equipo B al minuto 43 -> 2 suscriptores
[P] A-B 7 Tarjeta roja al numero 4 de Equipo A al minuto 50 -> 1 suscriptores
[P] C-D 7 Tarjeta roja al numero 4 de Equipo A al minuto 50 -> 2 suscriptores
[P] A-B 8 Atajada del portero de Equipo B al minuto 57 -> 1 suscriptores
[P] C-D 8 Atajada del portero de Equipo B al minuto 57 -> 2 suscriptores
[P] A-B 9 Gol de Equipo A al minuto 64 -> 1 suscriptores
[P] C-D 9 Gol de Equipo A al minuto 64 -> 2 suscriptores
[P] A-B 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71 -> 1 suscriptores
[P] C-D 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71 -> 2 suscriptores
[-] cliente 2 desconectado (cerro la conexion)
[-] cliente 3 desconectado (cerro la conexion)
[-] cliente 1 desconectado (cerro la conexion)
[-] cliente 0 desconectado (cerro la conexion)
[*] broker detenido
```

### Suscriptor 1 (A-B y C-D)

```
[quic] conectado: QUICv1, TLS_AES_256_GCM_SHA384
A-B 1 Gol de Equipo A al minuto 8
C-D 1 Gol de Equipo A al minuto 8
A-B 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15
C-D 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15
A-B 3 Cambio: jugador 10 entra por jugador 20 al minuto 22
C-D 3 Cambio: jugador 10 entra por jugador 20 al minuto 22
A-B 4 Tiro de esquina para Equipo B al minuto 29
C-D 4 Tiro de esquina para Equipo B al minuto 29
A-B 5 Fuera de lugar de Equipo A al minuto 36
C-D 5 Fuera de lugar de Equipo A al minuto 36
A-B 6 Gol de Equipo B al minuto 43
C-D 6 Gol de Equipo B al minuto 43
A-B 7 Tarjeta roja al numero 4 de Equipo A al minuto 50
C-D 7 Tarjeta roja al numero 4 de Equipo A al minuto 50
A-B 8 Atajada del portero de Equipo B al minuto 57
C-D 8 Atajada del portero de Equipo B al minuto 57
A-B 9 Gol de Equipo A al minuto 64
C-D 9 Gol de Equipo A al minuto 64
A-B 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71
C-D 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71
```

### Suscriptor 2 (C-D)

```
[quic] conectado: QUICv1, TLS_AES_256_GCM_SHA384
C-D 1 Gol de Equipo A al minuto 8
C-D 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15
C-D 3 Cambio: jugador 10 entra por jugador 20 al minuto 22
C-D 4 Tiro de esquina para Equipo B al minuto 29
C-D 5 Fuera de lugar de Equipo A al minuto 36
C-D 6 Gol de Equipo B al minuto 43
C-D 7 Tarjeta roja al numero 4 de Equipo A al minuto 50
C-D 8 Atajada del portero de Equipo B al minuto 57
C-D 9 Gol de Equipo A al minuto 64
C-D 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71
```

### Publicador 1 (A-B)

```
[quic] conectado: QUICv1, TLS_AES_256_GCM_SHA384
enviado: A-B 1 Gol de Equipo A al minuto 8
enviado: A-B 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15
enviado: A-B 3 Cambio: jugador 10 entra por jugador 20 al minuto 22
enviado: A-B 4 Tiro de esquina para Equipo B al minuto 29
enviado: A-B 5 Fuera de lugar de Equipo A al minuto 36
enviado: A-B 6 Gol de Equipo B al minuto 43
enviado: A-B 7 Tarjeta roja al numero 4 de Equipo A al minuto 50
enviado: A-B 8 Atajada del portero de Equipo B al minuto 57
enviado: A-B 9 Gol de Equipo A al minuto 64
enviado: A-B 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71
```

### Publicador 2 (C-D)

```
[quic] conectado: QUICv1, TLS_AES_256_GCM_SHA384
enviado: C-D 1 Gol de Equipo A al minuto 8
enviado: C-D 2 Tarjeta amarilla al numero 10 de Equipo B al minuto 15
enviado: C-D 3 Cambio: jugador 10 entra por jugador 20 al minuto 22
enviado: C-D 4 Tiro de esquina para Equipo B al minuto 29
enviado: C-D 5 Fuera de lugar de Equipo A al minuto 36
enviado: C-D 6 Gol de Equipo B al minuto 43
enviado: C-D 7 Tarjeta roja al numero 4 de Equipo A al minuto 50
enviado: C-D 8 Atajada del portero de Equipo B al minuto 57
enviado: C-D 9 Gol de Equipo A al minuto 64
enviado: C-D 10 Tarjeta amarilla al numero 10 de Equipo B al minuto 71
```
