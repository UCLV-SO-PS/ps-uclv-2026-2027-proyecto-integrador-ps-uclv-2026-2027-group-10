#define _POSIX_C_SOURCE 200809L

#include "bd_episodios.h"
#include "monitor_util.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define HOST_POR_DEFECTO "127.0.0.1"
#define PUERTO_POR_DEFECTO "6666"
#define TIMEOUT_CLIENTE_SEGUNDOS 5
#define BUFFER_RESPUESTA 2048

static void imprimir_ayuda(const char *programa)
{
    printf("Uso: %s [opciones] [codigo]\n", programa);
    printf("  codigo           Codigo de episodio, por ejemplo S01E01.\n");
    printf("  -H, --host H     Host del servidor (por defecto %s).\n", HOST_POR_DEFECTO);
    printf("  -p, --port P     Puerto del servidor (por defecto %s).\n", PUERTO_POR_DEFECTO);
    printf("  -h, --help       Muestra esta ayuda.\n");
    printf("\nSin codigo se inicia el modo interactivo.\n");
}

static int puerto_valido(const char *texto)
{
    char *fin = NULL;
    long valor;

    if (texto == NULL || *texto == '\0') {
        return 0;
    }

    errno = 0;
    valor = strtol(texto, &fin, 10);
    return errno == 0 && fin != texto && *fin == '\0' && valor >= 1 && valor <= 65535;
}

/* Quita espacios y saltos de linea sobrantes de la entrada interactiva. */
static void recortar(char *texto)
{
    size_t largo = strlen(texto);
    char *inicio = texto;

    while (largo > 0) {
        char ultimo = texto[largo - 1];
        if (ultimo != '\n' && ultimo != '\r' && ultimo != ' ' && ultimo != '\t') {
            break;
        }
        texto[--largo] = '\0';
    }

    while (*inicio == ' ' || *inicio == '\t') {
        inicio++;
    }

    if (inicio != texto) {
        memmove(texto, inicio, strlen(inicio) + 1);
    }
}

static void configurar_timeout(int descriptor)
{
    struct timeval tiempo;

    tiempo.tv_sec = TIMEOUT_CLIENTE_SEGUNDOS;
    tiempo.tv_usec = 0;

    (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &tiempo, sizeof(tiempo));
    (void)setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &tiempo, sizeof(tiempo));
}

static int consultar(const char *host, const char *puerto, const char *codigo_original)
{
    struct addrinfo pistas;
    struct addrinfo *resultado = NULL;
    struct addrinfo *actual;
    char codigo[BD_CODIGO_LEN];
    char peticion[16];
    char respuesta[BUFFER_RESPUESTA];
    size_t usados = 0;
    int descriptor = -1;
    int estado;
    int largo;

    /* Valida y normaliza el codigo antes de abrir la conexion. */
    if (!monitor_validar_codigo(codigo_original)) {
        fprintf(stderr, "Codigo invalido: '%s'. Use el formato S01E01.\n", codigo_original);
        return 1;
    }

    memcpy(codigo, codigo_original, strlen(codigo_original) + 1);
    monitor_normalizar_codigo(codigo);

    memset(&pistas, 0, sizeof(pistas));
    pistas.ai_family = AF_UNSPEC;
    pistas.ai_socktype = SOCK_STREAM;

    estado = getaddrinfo(host, puerto, &pistas, &resultado);
    if (estado != 0) {
        fprintf(stderr, "No se pudo resolver %s:%s: %s\n", host, puerto, gai_strerror(estado));
        return 1;
    }

    /* Prueba cada direccion devuelta por getaddrinfo hasta conectar. */
    for (actual = resultado; actual != NULL; actual = actual->ai_next) {
        descriptor = socket(actual->ai_family, actual->ai_socktype, actual->ai_protocol);
        if (descriptor < 0) {
            continue;
        }

        if (connect(descriptor, actual->ai_addr, actual->ai_addrlen) == 0) {
            break;
        }

        close(descriptor);
        descriptor = -1;
    }

    freeaddrinfo(resultado);

    if (descriptor < 0) {
        fprintf(stderr, "No se pudo conectar al servidor %s:%s\n", host, puerto);
        return 1;
    }

    configurar_timeout(descriptor);

    largo = snprintf(peticion, sizeof(peticion), "%s\n", codigo);
    if (largo < 0 || monitor_enviar_todo(descriptor, peticion, (size_t)largo) < 0) {
        fprintf(stderr, "Error enviando la consulta: %s\n", strerror(errno));
        close(descriptor);
        return 1;
    }

    /* Avisa que la solicitud termino; el servidor responde y cierra. */
    shutdown(descriptor, SHUT_WR);

    /* Lee hasta EOF. */
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
            fprintf(stderr, "Error leyendo la respuesta: %s\n", strerror(errno));
            close(descriptor);
            return 1;
        }

        usados += (size_t)leidos;
        if (usados >= sizeof(respuesta) - 1) {
            break;
        }
    }

    close(descriptor);

    if (usados == 0) {
        fprintf(stderr, "El servidor no envio respuesta.\n");
        return 1;
    }

    respuesta[usados] = '\0';
    fputs(respuesta, stdout);
    fflush(stdout);

    /* 0 si la consulta fue correcta; 2 si el servidor respondio con ERROR. */
    return strncmp(respuesta, "OK", 2) == 0 ? 0 : 2;
}

int main(int argc, char **argv)
{
    const char *host = HOST_POR_DEFECTO;
    const char *puerto = PUERTO_POR_DEFECTO;
    const char *codigo = NULL;
    char linea[128];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            imprimir_ayuda(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--host") == 0 || strcmp(argv[i], "-H") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Falta el valor de --host.\n");
                return 1;
            }
            host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) {
            if (i + 1 >= argc || !puerto_valido(argv[i + 1])) {
                fprintf(stderr, "Puerto invalido. Use un valor entre 1 y 65535.\n");
                return 1;
            }
            puerto = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Opcion desconocida: %s\n", argv[i]);
            imprimir_ayuda(argv[0]);
            return 1;
        } else if (codigo == NULL) {
            codigo = argv[i];
        } else {
            fprintf(stderr, "Solo se permite un codigo por invocacion.\n");
            return 1;
        }
    }

    if (codigo != NULL) {
        return consultar(host, puerto, codigo);
    }

    /* Sin codigo en argv: modo interactivo, una conexion por consulta. */
    printf("Cliente IMDb. Escriba un codigo (S01E01) o 'salir'.\n");

    for (;;) {
        printf("codigo> ");
        fflush(stdout);

        if (fgets(linea, sizeof(linea), stdin) == NULL) {
            break;
        }

        recortar(linea);

        if (linea[0] == '\0') {
            continue;
        }
        if (strcmp(linea, "salir") == 0 ||
            strcmp(linea, "quit") == 0 ||
            strcmp(linea, "exit") == 0) {
            break;
        }

        (void)consultar(host, puerto, linea);
    }

    return 0;
}
