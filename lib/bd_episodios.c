#include "bd_episodios.h"
#include "monitor_util.h"

#include <string.h>

/* Tabla inmutable: los hijos la heredan con fork() sin IPC ni copias. */
static const Episodio bd_episodios[] = {
    {"S01E01", "The Hedge Knight", 9.2, 45120UL},
    {"S01E02", "Hard Salt Beef", 8.8, 38900UL},
    {"S01E03", "The Squire", 9.5, 52400UL},
    {"S01E04", "Seven", 9.1, 41200UL},
    {"S01E05", "In the Name of the Mother", 9.7, 68300UL},
    {"S01E06", "The Morrow", 9.4, 49800UL},
};

static const size_t bd_cantidad = sizeof(bd_episodios) / sizeof(bd_episodios[0]);

static void escribir_error(char *error, size_t n, const char *mensaje)
{
    size_t largo;

    if (error == NULL || n == 0) {
        return;
    }

    largo = strlen(mensaje);
    if (largo >= n) {
        largo = n - 1;
    }

    memcpy(error, mensaje, largo);
    error[largo] = '\0';
}

/* Revisa integridad y duplicados antes de que el servidor acepte clientes. */
int bd_validar_tabla(char *error, size_t n)
{
    if (error != NULL && n > 0) {
        error[0] = '\0';
    }

    if (bd_cantidad == 0) {
        escribir_error(error, n, "la tabla de episodios esta vacia");
        return -1;
    }

    for (size_t i = 0; i < bd_cantidad; i++) {
        const Episodio *episodio = &bd_episodios[i];

        if (!monitor_validar_codigo(episodio->codigo)) {
            escribir_error(error, n, "codigo de episodio invalido");
            return -1;
        }

        if (episodio->titulo[0] == '\0') {
            escribir_error(error, n, "titulo de episodio vacio");
            return -1;
        }

        if (episodio->puntuacion < 0.0 || episodio->puntuacion > 10.0) {
            escribir_error(error, n, "puntuacion fuera de rango");
            return -1;
        }

        for (size_t j = i + 1; j < bd_cantidad; j++) {
            if (strcmp(episodio->codigo, bd_episodios[j].codigo) == 0) {
                escribir_error(error, n, "codigo de episodio duplicado");
                return -1;
            }
        }
    }

    return 0;
}

int bd_inicializar(void)
{
    return bd_validar_tabla(NULL, 0);
}

size_t bd_total(void)
{
    return bd_cantidad;
}

const Episodio *bd_obtener(size_t indice)
{
    if (indice >= bd_cantidad) {
        return NULL;
    }

    return &bd_episodios[indice];
}

const Episodio *bd_buscar_por_codigo(const char *codigo)
{
    char normalizado[BD_CODIGO_LEN];
    size_t largo;

    if (!monitor_validar_codigo(codigo)) {
        return NULL;
    }

    largo = strlen(codigo);
    memcpy(normalizado, codigo, largo + 1);
    monitor_normalizar_codigo(normalizado);

    /* Compara con el codigo normalizado: s01e01 encuentra S01E01. */
    for (size_t i = 0; i < bd_cantidad; i++) {
        if (strcmp(bd_episodios[i].codigo, normalizado) == 0) {
            return &bd_episodios[i];
        }
    }

    return NULL;
}
