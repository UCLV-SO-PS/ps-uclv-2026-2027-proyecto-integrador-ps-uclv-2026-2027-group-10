# Servidor Concurrente TCP de Calificaciones IMDb. Programación de Sistemas.

### Nestor Nuñez Díaz

### Adriana Isabel Acosta González

### Nota: 5

---

## 1. Descripción

Servidor concurrente TCP en C (POSIX/C11) que escucha en el puerto 6666 y responde consultas sobre los episodios de la primera temporada. Cada conexión se atiende en un proceso hijo creado con `fork()`; la base de datos y las utilidades de validación, lectura y formato viven en la biblioteca estática **`libmonitor.a`**.

El sistema tiene dos ejecutables:

- `server`: acepta conexiones, crea un hijo por cliente, recibe la IP del cliente por una tubería y cosecha los hijos terminados.
- `client`: consola de prueba con modo directo e interactivo.

---

## 2. Arquitectura

```
                        TCP :6666
 Cliente ───────────────────────────────► Proceso padre
    ▲                                         │
    │                                    accept()
    │                                         │
    │                                    pipe() + fork()
    │                                         │
    │                                         ├──────────► Proceso hijo
    │                                         │                 │
    │                                         │           escribe en el pipe
    │                                         │           lee, valida y busca
    │                                         │           en libmonitor.a
    │  ◄───────── respuesta formateada ───────┼─────────────────┘
    │                                         │
    │                                    SIGCHLD
    │                                         │
    │                            waitpid(-1, NULL, WNOHANG)
```

Flujo de una conexión:

1. El padre hace `accept()` y crea la tubería **antes** del `fork()`, para que el hijo tenga el extremo de escritura.
2. El hijo cierra el socket de escucha, envía `ip\n` al padre y atiende al cliente.
3. El padre cierra el socket del cliente, lee la IP con `poll()` y registra `Cliente <ip> atendido por PID <pid>`.
4. Al terminar el hijo, `SIGCHLD` cosecha los zombies con `waitpid(..., WNOHANG)`.

---

## 3. Estructura del repositorio

```
.
├── Makefile
├── server.c
├── client.c
├── include/
│   ├── bd_episodios.h
│   └── monitor_util.h
├── lib/
│   ├── bd_episodios.c
│   └── monitor_util.c
├── screenshot/
│   ├── Ejemplo.png
│   └── Ejemplo 2.png
├── libmonitor.a          (generado por make)
├── server                (generado por make)
├── client                (generado por make)
└── README.md
```

---

## 4. Requisitos y compilación

- Linux o WSL.
- `gcc` con soporte C11, `make` y `ar`.

```bash
sudo apt install build-essential netcat-openbsd iproute2
make clean && make all
```

`make all` compila `lib/*.c`, empaqueta los objetos en `libmonitor.a` con `ar rcs` y enlaza `server` y `client`.

---

## 5. Uso

### Servidor

```bash
./server                  # puerto 6666
./server --port 7000
./server --test-run       # autoverificación, termina con código 0
kill -TERM <PID>          # cierre ordenado
```

`Ctrl+C` se ignora por requisito; el servidor solo se detiene con `SIGTERM`.

### Cliente

```bash
./client S01E01
./client -H 127.0.0.1 -p 6666 s01e03
./client                  # modo interactivo; 'salir' para terminar
```

### Respuesta

```
OK
Codigo: S01E01
Titulo: The Hedge Knight
Puntuacion IMDb: 9.2
Votos: 45,120
```

Errores posibles: `ERROR: episodio no encontrado`, `ERROR: formato de codigo invalido`, `ERROR: codigo demasiado largo`, `ERROR: solicitud vacia o conexion cerrada`.

Códigos de salida del cliente: `0` consulta correcta, `1` error de uso/conexión, `2` respuesta de error del servidor.

---

## 6. Decisiones de diseño

Se decidió separar el código en módulos para concentrar responsabilidades: `bd_episodios` contiene los datos y la búsqueda, `monitor_util` reúne validación, normalización, formateo y E/S robusta, mientras que `client.c` y `server.c` implementan la lógica de red y la interfaz de usuario, evitando duplicar reglas entre cliente y servidor. El servidor se implementó como concurrente mediante `fork`, de modo que cada conexión aceptada se atiende en un proceso hijo y el padre vuelve rápidamente a `accept`. La IP del cliente se obtiene en el hijo con `inet_ntop` y se envía al padre por una tubería antes de procesar la solicitud; el padre la lee con un tiempo máximo de espera de un segundo y registra el cliente atendido, usando `desconocida` si la IP no llega a tiempo o no se puede resolver. En cuanto a señales, se ignora `SIGINT` para que `Ctrl+C` no detenga el servidor, se ignora `SIGPIPE` para evitar terminaciones abruptas al escribir a clientes desconectados, se atiende `SIGCHLD` con `waitpid(-1, ..., WNOHANG)` para cosechar hijos terminados y evitar zombis, y `SIGTERM` marca el servidor como inactivo para permitir una parada ordenada; además, en `--test-run` se usa `SIGALRM` con `alarm(10)` como protección contra bloqueos. Cliente y servidor configuran timeouts de cinco segundos para recepción y envío, y las funciones de E/S reintentan ante `EINTR`; el servidor rechaza códigos de más de seis caracteres usando `poll` y `MSG_PEEK` para no quedarse bloqueado esperando un delimitador. Los códigos deben cumplir exactamente el formato `SxxExx`, con dos dígitos de temporada y dos de episodio, se aceptan minúsculas y se normalizan a mayúsculas antes de buscar en la base de datos. La petición es el código seguido de `\n`, la respuesta correcta comienza con `OK\n` y los errores con `ERROR: <motivo>\n`, y el cliente cierra el sentido de escritura con `shutdown(SHUT_WR)` después de enviar. El servidor incluye un modo `--test-run` que inicializa y valida la base de datos, abre el socket, lanza una sonda interna contra `127.0.0.1`, envía `S01E01`, verifica que la respuesta contenga `OK`, `S01E01` y `9.2`, comprueba la recepción de la IP por la tubería y valida los códigos de salida de la sonda y del hijo que atendió la conexión, permitiendo validar la integración sin dejar el servidor en ejecución. El cliente usa `getaddrinfo` con `AF_UNSPEC` para resolver nombres IPv4 o IPv6, aunque el servidor escucha solo en IPv4, permite consulta directa o modo interactivo, recorta espacios y acepta los comandos `salir`, `quit` y `exit`; devuelve código de salida `0` cuando la respuesta comienza con `OK`, `2` cuando el servidor responde con `ERROR` y `1` ante errores locales, de validación o de conexión. La base de datos es estática e interna, manteniendo la estructura del ejercicio.

---

## 7. Pruebas y verificación

```bash
make test          # equivale a ./server --test-run
```

Comprobaciones realizadas durante el desarrollo:

| Prueba                            | Resultado esperado                             |
| --------------------------------- | ---------------------------------------------- |
| `--test-run`                      | Código 0, socket e IP por tubería funcionando  |
| Consultas S01E01–S01E06           | Título, puntuación y votos correctos           |
| Código en minúsculas (`s01e05`)   | Se normaliza y responde                        |
| Código inexistente                | `ERROR: episodio no encontrado`                |
| Formato inválido                  | `ERROR: formato de codigo invalido`            |
| Código de más de 6 caracteres     | `ERROR: codigo demasiado largo`                |
| Solicitud sin `\n` o solo 6 bytes | Se atiende correctamente                       |
| `Ctrl+C`                          | El servidor sigue vivo                         |
| Varios clientes concurrentes      | Todos reciben respuesta                        |
| `kill -TERM`                      | Cierre ordenado y mensaje `Servidor detenido.` |

---
