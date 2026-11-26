#ifndef BD_EPISODIOS_H
#define BD_EPISODIOS_H

#include <stddef.h>

#define BD_CODIGO_LEN 7
#define BD_TITULO_LEN 64

typedef struct {
    char codigo[BD_CODIGO_LEN];
    char titulo[BD_TITULO_LEN];
    double puntuacion;
    unsigned long votos;
} Episodio;

/* Inicializa y valida la base de datos interna. Devuelve 0 si es correcta. */
int bd_inicializar(void);

/* Valida la tabla completa. Si error != NULL, deja un mensaje descriptivo. */
int bd_validar_tabla(char *error, size_t n);

/* Cantidad de episodios registrados. */
size_t bd_total(void);

/* Episodio por indice, o NULL si el indice esta fuera de rango. */
const Episodio *bd_obtener(size_t indice);

/* Busqueda por codigo (acepta minusculas). NULL si no existe. */
const Episodio *bd_buscar_por_codigo(const char *codigo);

#endif /* BD_EPISODIOS_H */
