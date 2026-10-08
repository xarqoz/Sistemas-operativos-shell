#ifndef PLAYLIST_H
#define PLAYLIST_H

/*
 * ====================================================================================
 * MÓDULO: playlist (Gestión Concurrente de la Lista de Reproducción)
 * ====================================================================================
 * Implementa una lista de reproducción compartida y segura para concurrencia mediante
 * el patrón clásico LECTORES-ESCRITORES usando pthread_rwlock_t.
 *
 *   - LECTURAS (varias simultáneas): consultar la pista actual, listar la cola, obtener
 *     la ruta de la siguiente canción. El hilo de reproducción (productor) es un lector
 *     frecuente.
 *   - ESCRITURAS (exclusivas): agregar, eliminar, mover (reordenar) y limpiar la lista.
 *     Normalmente las realiza el usuario desde el hilo de la CLI.
 *
 * El rwlock permite que múltiples lectores accedan a la vez, pero da acceso EXCLUSIVO a
 * un escritor, evitando que se lea un nodo mientras se libera su memoria (uso después de
 * liberar / segmentation fault), que es exactamente el riesgo que señala el enunciado.
 *
 * Decisión de diseño: la lista es DOBLEMENTE ENLAZADA para poder mover nodos y navegar
 * hacia atrás (Previous) en O(1) por nodo. El "cursor" (nodo actual) se guarda dentro de
 * la estructura y también está protegido por el mismo rwlock.
 * ==================================================================================== */

#include <pthread.h>
#include <stddef.h>

#define PL_MAX_PATH 1024   /* Longitud máxima de la ruta de una canción */
#define PL_MAX_NAME 256    /* Longitud máxima del nombre visible de la pista */

/* Nodo de la lista doblemente enlazada: representa una canción en la cola. */
typedef struct Track {
    char path[PL_MAX_PATH];   /* Ruta en disco del archivo de audio */
    char name[PL_MAX_NAME];   /* Nombre legible (derivado del nombre de archivo) */
    struct Track *prev;       /* Nodo anterior en la cola */
    struct Track *next;       /* Nodo siguiente en la cola */
} Track;

/* Estructura principal de la playlist compartida entre hilos. */
typedef struct {
    Track *head;              /* Primer nodo de la cola */
    Track *tail;              /* Último nodo de la cola */
    Track *current;           /* Cursor: pista "seleccionada"/en reproducción */
    int    count;             /* Número de canciones en la cola */

    pthread_rwlock_t lock;    /* Candado lectores-escritores que protege TODA la estructura */
} Playlist;

/* --- Ciclo de vida --- */
void playlist_init(Playlist *pl);
void playlist_destroy(Playlist *pl);   /* Libera todos los nodos y el rwlock */

/* --- Operaciones de ESCRITURA (toman el lock en modo escritura) --- */
int  playlist_add(Playlist *pl, const char *path);          /* Agrega al final. Devuelve índice o -1 */
int  playlist_remove(Playlist *pl, int index);              /* Elimina la pista en 'index' (0-based) */
int  playlist_move(Playlist *pl, int from, int to);         /* Reordena: mueve 'from' a la posición 'to' */
void playlist_clear(Playlist *pl);                          /* Vacía la cola completa */

/* --- Navegación del cursor (ESCRITURA del campo current) --- */
int  playlist_select(Playlist *pl, int index);             /* Fija el cursor en 'index' */
int  playlist_next(Playlist *pl);                           /* Avanza el cursor; devuelve 1 si pudo */
int  playlist_prev(Playlist *pl);                           /* Retrocede el cursor; devuelve 1 si pudo */

/* --- Operaciones de LECTURA (toman el lock en modo lectura) --- */
/* Copia la ruta y el nombre de la pista actual a los búferes dados. Devuelve 1 si hay
 * pista actual, 0 si la cola está vacía. Copiar (en vez de devolver el puntero) evita
 * que el llamador use un nodo que otro hilo podría liberar. */
int  playlist_get_current(Playlist *pl, char *path_out, size_t path_sz,
                          char *name_out, size_t name_sz, int *index_out);
int  playlist_count(Playlist *pl);                          /* Número de pistas (lectura) */
void playlist_print(Playlist *pl);                          /* Imprime la cola marcando la actual */

#endif /* PLAYLIST_H */
