#define _POSIX_C_SOURCE 200809L

#include "bd_episodios.h"
#include "monitor_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Parametros de red y tamano de los buffers de trabajo. */
#define PUERTO_POR_DEFECTO 6666
#define BUFFER_SOLICITUD 64
#define BUFFER_RESPUESTA 512
#define TIMEOUT_CLIENTE_SEGUNDOS 5
#define TIMEOUT_IP_MILISEGUNDOS 1000

/* Bandera que SIGTERM pone en 0 para salir del bucle principal. */
static volatile sig_atomic_t servidor_activo = 1;

/* SIGTERM: solo marca la salida; el cierre se hace en main(). */
static void manejador_terminacion(int senal)
{
    (void)senal;
    servidor_activo = 0;
}

static void manejador_alarma(int senal)
{
    (void)senal;
}

/* SIGCHLD: recolecta hijos terminados para evitar zombis. */
static void cosechar_hijos(int senal)
{
    int error_guardado = errno;
    (void)senal;

    for (;;) {
        pid_t resultado = waitpid(-1, NULL, WNOHANG);

        if (resultado > 0) {
            continue;
        }
        if (resultado < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    errno = error_guardado;
}

static int ignorar_sigint_y_sigpipe(void)
{
    struct sigaction accion;

    memset(&accion, 0, sizeof(accion));
    sigemptyset(&accion.sa_mask);
    accion.sa_handler = SIG_IGN;  // Ignoramos

    if (sigaction(SIGINT, &accion, NULL) < 0) {
        return -1;
    }
    if (sigaction(SIGPIPE, &accion, NULL) < 0) {
        return -1;
    }

    return 0;
}

static int instalar_senales(void)
{
    struct sigaction accion;

    if (ignorar_sigint_y_sigpipe() < 0) {
        return -1;
    }

    memset(&accion, 0, sizeof(accion));
    sigemptyset(&accion.sa_mask);
    accion.sa_handler = cosechar_hijos; // Para cosechar los hijos cuando se lanza SIGCHLD
    accion.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &accion, NULL) < 0) {
        return -1;
    }

    memset(&accion, 0, sizeof(accion));
    sigemptyset(&accion.sa_mask);
    accion.sa_handler = manejador_terminacion;
    if (sigaction(SIGTERM, &accion, NULL) < 0) {
        return -1;
    }

    return 0;
}

static int instalar_alarma(void)
{
    struct sigaction accion;

    memset(&accion, 0, sizeof(accion));
    sigemptyset(&accion.sa_mask);
    accion.sa_handler = manejador_alarma;
    return sigaction(SIGALRM, &accion, NULL);
}

static int parsear_puerto(const char *texto, int *puerto)
{
    char *fin = NULL;
    long valor;

    if (texto == NULL || *texto == '\0') {
        return -1;
    }

    errno = 0;
    valor = strtol(texto, &fin, 10);
    if (errno != 0 || fin == texto || *fin != '\0' || valor < 1 || valor > 65535) {
        return -1;
    }

    *puerto = (int)valor;
    return 0;
}

static int crear_socket_escucha(int puerto)
{
    int descriptor;
    int activo = 1;
    struct sockaddr_in direccion;

    descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return -1;
    }

    /* Permite reiniciar el servidor sin esperar a que el puerto quede libre. */
    if (setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &activo, sizeof(activo)) < 0) {
        int error_guardado = errno;
        close(descriptor);
        errno = error_guardado;
        return -1;
    }

    memset(&direccion, 0, sizeof(direccion));
    direccion.sin_family = AF_INET;
    direccion.sin_addr.s_addr = htonl(INADDR_ANY);
    direccion.sin_port = htons((uint16_t)puerto);

    if (bind(descriptor, (struct sockaddr *)&direccion, sizeof(direccion)) < 0) {
        int error_guardado = errno;
        close(descriptor);
        errno = error_guardado;
        return -1;
    }

    if (listen(descriptor, SOMAXCONN) < 0) {
        int error_guardado = errno;
        close(descriptor);
        errno = error_guardado;
        return -1;
    }

    return descriptor;
}

/* Evita que un cliente inactivo deje al hijo bloqueado para siempre. */
static void configurar_timeouts(int descriptor)
{
    struct timeval tiempo;

    tiempo.tv_sec = TIMEOUT_CLIENTE_SEGUNDOS;
    tiempo.tv_usec = 0;

    (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &tiempo, sizeof(tiempo));
    (void)setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &tiempo, sizeof(tiempo));
}

static void poner_desconocida(char *ip, size_t n)
{
    const char *texto = "desconocida";
    size_t largo;

    if (ip == NULL || n == 0) {
        return;
    }

    largo = strlen(texto);
    if (largo >= n) {
        largo = n - 1;
    }

    memcpy(ip, texto, largo);
    ip[largo] = '\0';
}

/* El hijo envia su IP al padre y cierra el pipe para que el padre vea el EOF. */
static void enviar_ip_al_padre(int descriptor, const struct sockaddr_in *cliente)
{
    char ip[INET_ADDRSTRLEN];
    char linea[INET_ADDRSTRLEN + 2];
    int largo;

    if (inet_ntop(AF_INET, &cliente->sin_addr, ip, sizeof(ip)) == NULL) {
        poner_desconocida(ip, sizeof(ip));
    }

    largo = snprintf(linea, sizeof(linea), "%s\n", ip);
    if (largo > 0) {
        ssize_t escritos = write(descriptor, linea, (size_t)largo);
        (void)escritos;
    }

    close(descriptor);
}

/* Flujo del hijo: avisa la IP, lee y valida el codigo, responde y cierra. */
static void atender_cliente(int descriptor, int tuberia_escritura,
                            const struct sockaddr_in *cliente)
{
    char solicitud[BUFFER_SOLICITUD];
    char respuesta[BUFFER_RESPUESTA];
    ssize_t leidos;

    enviar_ip_al_padre(tuberia_escritura, cliente);
    configurar_timeouts(descriptor);

    leidos = monitor_leer_codigo(descriptor, solicitud, sizeof(solicitud));

    /* -2: el cliente envio mas de 6 caracteres. */
    if (leidos == -2) {
        (void)monitor_formatear_error("codigo demasiado largo", respuesta, sizeof(respuesta));
    } else if (leidos <= 0) {
        (void)monitor_formatear_error("solicitud vacia o conexion cerrada", respuesta,
                                      sizeof(respuesta));
    } else if (!monitor_validar_codigo(solicitud)) {
        (void)monitor_formatear_error("formato de codigo invalido", respuesta, sizeof(respuesta));
    } else {
        const Episodio *episodio;

        monitor_normalizar_codigo(solicitud);
        episodio = bd_buscar_por_codigo(solicitud);

        if (episodio == NULL) {
            (void)monitor_formatear_error("episodio no encontrado", respuesta, sizeof(respuesta));
        } else if (monitor_formatear_episodio(episodio, respuesta, sizeof(respuesta)) < 0) {
            (void)monitor_formatear_error("error interno al formatear", respuesta,
                                          sizeof(respuesta));
        }
    }

    (void)monitor_enviar_todo(descriptor, respuesta, strlen(respuesta));
    shutdown(descriptor, SHUT_WR);
    close(descriptor);
    _exit(0);
}

/* Lee la IP con timeout para que el padre no se bloquee en accept(). */
static void leer_ip_del_pipe(int descriptor, char *ip, size_t n)
{
    struct pollfd espera;
    int listo;
    size_t usados = 0;

    if (ip == NULL || n == 0) {
        return;
    }

    ip[0] = '\0';
    espera.fd = descriptor;
    espera.events = POLLIN;
    espera.revents = 0;

    do {
        listo = poll(&espera, 1, TIMEOUT_IP_MILISEGUNDOS);
    } while (listo < 0 && errno == EINTR);

    if (listo <= 0) {
        poner_desconocida(ip, n);
        return;
    }

    for (;;) {
        char byte;
        ssize_t leidos = read(descriptor, &byte, 1);

        if (leidos == 0) {
            break;
        }
        if (leidos < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (byte == '\n') {
            break;
        }
        if (usados + 1 < n) {
            ip[usados++] = byte;
        }
    }

    ip[usados] = '\0';
    if (usados == 0) {
        poner_desconocida(ip, n);
    }
}

static int atender_conexion(int socket_escucha)
{
    struct sockaddr_in cliente;
    socklen_t largo_cliente = sizeof(cliente);
    int socket_cliente;
    int tuberia[2];
    pid_t pid;
    char ip[INET_ADDRSTRLEN];

    socket_cliente = accept(socket_escucha, (struct sockaddr *)&cliente, &largo_cliente);
    if (socket_cliente < 0) {
        return -1;
    }

    /* La tuberia se crea antes del fork para que el hijo herede el extremo de escritura. */
    if (pipe(tuberia) < 0) {
        int error_guardado = errno;
        close(socket_cliente);
        errno = error_guardado;
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        int error_guardado = errno;
        close(socket_cliente);
        close(tuberia[0]);
        close(tuberia[1]);
        errno = error_guardado;
        return -1;
    }

    if (pid == 0) {
        /* Hijo: cierra lo que no necesita y atiende la conexion. */
        close(socket_escucha);
        close(tuberia[0]);
        atender_cliente(socket_cliente, tuberia[1], &cliente);
        _exit(0);
    }

    /* Padre: cierra el socket del cliente, espera la IP y registra la conexion. */
    close(socket_cliente);
    close(tuberia[1]);

    leer_ip_del_pipe(tuberia[0], ip, sizeof(ip));
    close(tuberia[0]);

    printf("Cliente %s atendido por PID %ld\n", ip, (long)pid);
    fflush(stdout);

    return 0;
}

static int conectar_sonda(int puerto)
{
    struct sockaddr_in destino;
    int descriptor = -1;

    memset(&destino, 0, sizeof(destino));
    destino.sin_family = AF_INET;
    destino.sin_port = htons((uint16_t)puerto);

    if (inet_pton(AF_INET, "127.0.0.1", &destino.sin_addr) != 1) {
        return -1;
    }

    for (int intento = 0; intento < 20; intento++) {
        struct timespec espera = {0, 50000000L};

        descriptor = socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor < 0) {
            return -1;
        }

        if (connect(descriptor, (struct sockaddr *)&destino, sizeof(destino)) == 0) {
            return descriptor;
        }

        close(descriptor);
        descriptor = -1;
        (void)nanosleep(&espera, NULL);
    }

    return -1;
}

/* Sonda de --test-run: consulta S01E01 y comprueba la respuesta. */
static int ejecutar_sonda(int puerto)
{
    const char peticion[] = "S01E01\n";
    char respuesta[BUFFER_RESPUESTA];
    size_t usados = 0;
    int descriptor = conectar_sonda(puerto);

    if (descriptor < 0) {
        fprintf(stderr, "[FALLO] La sonda no pudo conectar al servidor\n");
        return 1;
    }

    if (monitor_enviar_todo(descriptor, peticion, sizeof(peticion) - 1) < 0) {
        close(descriptor);
        return 1;
    }

    shutdown(descriptor, SHUT_WR);

    for (;;) {
        ssize_t leidos = read(descriptor, respuesta + usados,
                              sizeof(respuesta) - 1 - usados);

        if (leidos == 0) {
            break;
        }
        if (leidos < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(descriptor);
            return 1;
        }

        usados += (size_t)leidos;
        if (usados >= sizeof(respuesta) - 1) {
            break;
        }
    }

    respuesta[usados] = '\0';
    close(descriptor);

    if (strstr(respuesta, "OK") == NULL ||
        strstr(respuesta, "S01E01") == NULL ||
        strstr(respuesta, "9.2") == NULL) {
        fprintf(stderr, "[FALLO] Respuesta inesperada de la sonda:\n%s\n", respuesta);
        return 1;
    }

    return 0;
}

/* Prueba integrada: recorre socket, fork, tuberia y consulta real. */
static int modo_test_run(int puerto)
{
    int socket_escucha;
    int socket_cliente;
    int tuberia[2];
    struct sockaddr_in cliente;
    socklen_t largo_cliente = sizeof(cliente);
    pid_t sonda;
    pid_t hijo;
    char ip[INET_ADDRSTRLEN];
    int estado_hijo = 0;
    int estado_sonda = 0;
    int correcto = 1;

    if (instalar_alarma() < 0) {
        fprintf(stderr, "[FALLO] No se pudo instalar SIGALRM: %s\n", strerror(errno));
        return 1;
    }

    socket_escucha = crear_socket_escucha(puerto);
    if (socket_escucha < 0) {
        fprintf(stderr, "[FALLO] No se pudo abrir el socket en el puerto %d: %s\n",
                puerto, strerror(errno));
        return 1;
    }

    printf("[OK] Socket escuchando en 127.0.0.1:%d\n", puerto);
    fflush(stdout);

    sonda = fork();
    if (sonda < 0) {
        fprintf(stderr, "[FALLO] No se pudo crear la sonda: %s\n", strerror(errno));
        close(socket_escucha);
        return 1;
    }

    if (sonda == 0) {
        close(socket_escucha);
        _exit(ejecutar_sonda(puerto));
    }

    alarm(10);

    socket_cliente = accept(socket_escucha, (struct sockaddr *)&cliente, &largo_cliente);
    if (socket_cliente < 0) {
        fprintf(stderr, "[FALLO] accept fallo durante test-run: %s\n", strerror(errno));
        kill(sonda, SIGKILL);
        (void)waitpid(sonda, NULL, 0);
        close(socket_escucha);
        alarm(0);
        return 1;
    }

    if (pipe(tuberia) < 0) {
        fprintf(stderr, "[FALLO] No se pudo crear la tuberia: %s\n", strerror(errno));
        close(socket_cliente);
        kill(sonda, SIGKILL);
        (void)waitpid(sonda, NULL, 0);
        close(socket_escucha);
        alarm(0);
        return 1;
    }

    hijo = fork();
    if (hijo < 0) {
        fprintf(stderr, "[FALLO] No se pudo crear el hijo: %s\n", strerror(errno));
        close(socket_cliente);
        close(tuberia[0]);
        close(tuberia[1]);
        kill(sonda, SIGKILL);
        (void)waitpid(sonda, NULL, 0);
        close(socket_escucha);
        alarm(0);
        return 1;
    }

    if (hijo == 0) {
        close(socket_escucha);
        close(tuberia[0]);
        atender_cliente(socket_cliente, tuberia[1], &cliente);
        _exit(0);
    }

    close(socket_cliente);
    close(tuberia[1]);

    leer_ip_del_pipe(tuberia[0], ip, sizeof(ip));
    close(tuberia[0]);
    printf("[OK] IP recibida por la tuberia: %s (hijo PID %ld)\n", ip, (long)hijo);

    while (waitpid(hijo, &estado_hijo, 0) < 0 && errno == EINTR) {
        /* reintentar */
    }
    while (waitpid(sonda, &estado_sonda, 0) < 0 && errno == EINTR) {
        /* reintentar */
    }

    alarm(0);
    close(socket_escucha);

    if (!WIFEXITED(estado_hijo) || WEXITSTATUS(estado_hijo) != 0) {
        fprintf(stderr, "[FALLO] El hijo que atendio la sonda termino con error\n");
        correcto = 0;
    }

    if (!WIFEXITED(estado_sonda) || WEXITSTATUS(estado_sonda) != 0) {
        fprintf(stderr, "[FALLO] La sonda no valido la respuesta del servidor\n");
        correcto = 0;
    }

    if (!correcto) {
        return 1;
    }

    printf("[OK] Socket responde correctamente\n");
    printf("[OK] test-run completado\n");
    return 0;
}

static void imprimir_ayuda(const char *programa)
{
    printf("Uso: %s [opciones]\n", programa);
    printf("  --test-run     Inicializa la BD, valida el socket y termina con codigo 0.\n");
    printf("  --port N       Puerto TCP de escucha (por defecto %d).\n", PUERTO_POR_DEFECTO);
    printf("  -h, --help     Muestra esta ayuda.\n");
}

int main(int argc, char **argv)
{
    int puerto = PUERTO_POR_DEFECTO;
    int test_run = 0;
    int socket_escucha;
    char error_bd[128] = "";

    /* Opciones de linea de comandos. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test-run") == 0) {
            test_run = 1;
        } else if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc || parsear_puerto(argv[++i], &puerto) < 0) {
                fprintf(stderr, "Puerto invalido. Use un valor entre 1 y 65535.\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            imprimir_ayuda(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Opcion desconocida: %s\n", argv[i]);
            imprimir_ayuda(argv[0]);
            return 1;
        }
    }

    /* Valida la base de datos antes de aceptar clientes. */
    if (bd_inicializar() < 0) {
        (void)bd_validar_tabla(error_bd, sizeof(error_bd));
        fprintf(stderr, "Error en la base de datos: %s\n",
                error_bd[0] != '\0' ? error_bd : "tabla invalida");
        return 1;
    }

    if (test_run) {
        if (ignorar_sigint_y_sigpipe() < 0) {
            fprintf(stderr, "No se pudieron configurar las senales: %s\n", strerror(errno));
            return 1;
        }

        printf("[OK] Base de datos inicializada (%zu episodios)\n", bd_total());
        fflush(stdout);
        return modo_test_run(puerto);
    }

    if (instalar_senales() < 0) {
        fprintf(stderr, "No se pudieron configurar las senales: %s\n", strerror(errno));
        return 1;
    }

    socket_escucha = crear_socket_escucha(puerto);
    if (socket_escucha < 0) {
        fprintf(stderr, "No se pudo escuchar en el puerto %d: %s\n", puerto, strerror(errno));
        return 1;
    }

    printf("Servidor escuchando en 0.0.0.0:%d (PID %ld)\n", puerto, (long)getpid());
    printf("Ctrl+C sera ignorado. Use 'kill -TERM %ld' para detenerlo.\n", (long)getpid());
    fflush(stdout);

    /* Bucle principal: el padre solo acepta y delega en los hijos. */
    while (servidor_activo) {
        if (atender_conexion(socket_escucha) < 0) {
            if (errno == EINTR) {
                continue;
            }

            fprintf(stderr, "Error atendiendo una conexion: %s\n", strerror(errno));
            if (errno == EBADF || errno == EINVAL) {
                break;
            }
        }
    }

    close(socket_escucha);
    printf("Servidor detenido.\n");
    fflush(stdout);
    return 0;
}
