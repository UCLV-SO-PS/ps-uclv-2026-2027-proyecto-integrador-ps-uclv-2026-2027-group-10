#include "monitor_util.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Acepta exactamente SxxExx, con S y E en mayuscula o minuscula. */
int monitor_validar_codigo(const char *codigo)
{
    size_t largo;

    if (codigo == NULL) {
        return 0;
    }

    largo = strlen(codigo);
    if (largo != 6) {
        return 0;
    }

    if (codigo[0] != 'S' && codigo[0] != 's') {
        return 0;
    }
    if (!isdigit((unsigned char)codigo[1]) || !isdigit((unsigned char)codigo[2])) {
        return 0;
    }
    if (codigo[3] != 'E' && codigo[3] != 'e') {
        return 0;
    }
    if (!isdigit((unsigned char)codigo[4]) || !isdigit((unsigned char)codigo[5])) {
        return 0;
    }

    return 1;
}

/* Convierte el codigo a mayusculas para compararlo con la base. */
void monitor_normalizar_codigo(char *codigo)
{
    if (codigo == NULL) {
        return;
    }

    for (char *p = codigo; *p != '\0'; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
}

/* Inserta la coma de miles a mano para no depender del locale. */
void monitor_formatear_votos(unsigned long votos, char *buf, size_t n)
{
    char digitos[32];
    int escritos;
    size_t largo;
    size_t salida = 0;

    if (buf == NULL || n == 0) {
        return;
    }

    escritos = snprintf(digitos, sizeof(digitos), "%lu", votos);
    if (escritos < 0) {
        buf[0] = '\0';
        return;
    }

    largo = (size_t)escritos;
    for (size_t i = 0; i < largo; i++) {
        if (i > 0 && ((largo - i) % 3) == 0) {
            if (salida + 1 >= n) {
                buf[salida] = '\0';
                return;
            }
            buf[salida++] = ',';
        }

        if (salida + 1 >= n) {
            buf[salida] = '\0';
            return;
        }
        buf[salida++] = digitos[i];
    }

    buf[salida] = '\0';
}

ssize_t monitor_formatear_episodio(const Episodio *episodio, char *buf, size_t n)
{
    char votos[32];
    int escritos;

    if (episodio == NULL || buf == NULL || n == 0) {
        return -1;
    }

    monitor_formatear_votos(episodio->votos, votos, sizeof(votos));

    escritos = snprintf(buf, n,
                        "OK\n"
                        "Codigo: %s\n"
                        "Titulo: %s\n"
                        "Puntuacion IMDb: %.1f\n"
                        "Votos: %s\n",
                        episodio->codigo, episodio->titulo,
                        episodio->puntuacion, votos);

    if (escritos < 0 || (size_t)escritos >= n) {
        return -1;
    }

    return (ssize_t)escritos;
}

ssize_t monitor_formatear_error(const char *motivo, char *buf, size_t n)
{
    int escritos;

    if (motivo == NULL || buf == NULL || n == 0) {
        return -1;
    }

    escritos = snprintf(buf, n, "ERROR: %s\n", motivo);
    if (escritos < 0 || (size_t)escritos >= n) {
        return -1;
    }

    return (ssize_t)escritos;
}

/* Lee hasta 6 bytes; acepta '\n', '\r', EOF o 6 bytes sin salto. */
ssize_t monitor_leer_codigo(int fd, char *buf, size_t n)
{
    size_t usados = 0;

    if (buf == NULL || n < BD_CODIGO_LEN) {
        errno = EINVAL;
        return -1;
    }

    for (;;) {
        char byte;
        ssize_t leidos = read(fd, &byte, 1);

        if (leidos == 0) {
            break;
        }
        if (leidos < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (byte == '\n' || byte == '\r') {
            break;
        }

        buf[usados++] = byte;

        /* Con 6 bytes, mira si sobra alguno sin bloquearse. */
        if (usados == 6) {
            struct pollfd espera;
            char siguiente;
            ssize_t visto;
            int listo;

            espera.fd = fd;
            espera.events = POLLIN;
            espera.revents = 0;

            do {
                listo = poll(&espera, 1, 0);
            } while (listo < 0 && errno == EINTR);

            if (listo <= 0) {
                break;
            }

            visto = recv(fd, &siguiente, 1, MSG_PEEK);
            if (visto == 0) {
                break;
            }
            if (visto == 1 && (siguiente == '\n' || siguiente == '\r')) {
                char descartado;
                ssize_t consumido = read(fd, &descartado, 1);
                (void)consumido;
                break;
            }
            if (visto == 1) {
                buf[usados] = '\0';
                return -2;
            }
            break;
        }
    }

    buf[usados] = '\0';
    return (ssize_t)usados;
}

/* send() puede escribir menos de lo pedido: repite hasta completar. */
ssize_t monitor_enviar_todo(int fd, const char *buf, size_t n)
{
    size_t enviados = 0;

    if (buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    while (enviados < n) {
        ssize_t escritos = send(fd, buf + enviados, n - enviados, 0);

        if (escritos < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (escritos == 0) {
            errno = EPIPE;
            return -1;
        }

        enviados += (size_t)escritos;
    }

    return (ssize_t)enviados;
}

void monitor_log(const char *formato, ...)
{
    va_list argumentos;

    va_start(argumentos, formato);
    vfprintf(stdout, formato, argumentos);
    va_end(argumentos);
    fputc('\n', stdout);
    fflush(stdout);
}

void monitor_error(const char *formato, ...)
{
    va_list argumentos;

    va_start(argumentos, formato);
    vfprintf(stderr, formato, argumentos);
    va_end(argumentos);
    fputc('\n', stderr);
    fflush(stderr);
}
