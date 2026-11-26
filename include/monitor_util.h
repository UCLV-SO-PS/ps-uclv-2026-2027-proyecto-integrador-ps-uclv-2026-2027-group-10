#ifndef MONITOR_UTIL_H
#define MONITOR_UTIL_H

#include <stddef.h>
#include <sys/types.h>

#include "bd_episodios.h"

/* Valida el formato SxxExx (acepta minusculas). Devuelve 1 si es valido. */
int monitor_validar_codigo(const char *codigo);

/* Convierte el codigo a mayusculas in situ. */
void monitor_normalizar_codigo(char *codigo);

/* Formatea los votos con separador de miles, por ejemplo 45,120. */
void monitor_formatear_votos(unsigned long votos, char *buf, size_t n);

/* Genera la respuesta completa de un episodio. */
ssize_t monitor_formatear_episodio(const Episodio *episodio, char *buf, size_t n);

/* Genera una respuesta de error. */
ssize_t monitor_formatear_error(const char *motivo, char *buf, size_t n);

/* Lee una solicitud de codigo (hasta 6 caracteres, con o sin '\n').
 * Devuelve bytes leidos, 0 en EOF, -1 error, -2 si excede 6 caracteres. */
ssize_t monitor_leer_codigo(int fd, char *buf, size_t n);

/* Envia todos los bytes, reintentando ante EINTR. */
ssize_t monitor_enviar_todo(int fd, const char *buf, size_t n);

/* Registro por consola. */
void monitor_log(const char *formato, ...);
void monitor_error(const char *formato, ...);

#endif /* MONITOR_UTIL_H */
