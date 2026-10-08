#include "playlist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ====================================================================================
 * IMPLEMENTACIÓN DE LA PLAYLIST CONCURRENTE (patrón Lectores-Escritores)
 * ====================================================================================
 * Todas las funciones que MODIFICAN la estructura toman el candado en modo escritura
 * (pthread_rwlock_wrlock), garantizando acceso exclusivo. Las que solo CONSULTAN lo toman
 * en modo lectura (pthread_rwlock_rdlock), permitiendo concurrencia entre lectores.
 * ==================================================================================== */

/* Deriva un nombre legible a partir de la ruta (toma lo que sigue al último '/'). */
static void derive_name(const char *path, char *name_out, size_t sz) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    strncpy(name_out, base, sz - 1);
    name_out[sz - 1] = '\0';
}

/* Devuelve el nodo en la posición 'index' (0-based) SIN tomar el lock (uso interno). */
static Track *node_at(Playlist *pl, int index) {
    if (index < 0 || index >= pl->count) return NULL;
    Track *t = pl->head;
    for (int i = 0; i < index && t; i++) t = t->next;
    return t;
}

void playlist_init(Playlist *pl) {
    pl->head = pl->tail = pl->current = NULL;
    pl->count = 0;
    /* Inicializa el candado lectores-escritores con atributos por defecto */
    pthread_rwlock_init(&pl->lock, NULL);
}

void playlist_destroy(Playlist *pl) {
    pthread_rwlock_wrlock(&pl->lock);        /* Acceso exclusivo para liberar todo */
    Track *t = pl->head;
    while (t) {
        Track *nxt = t->next;
        free(t);                             /* Libera memoria dinámica de cada nodo */
        t = nxt;
    }
    pl->head = pl->tail = pl->current = NULL;
    pl->count = 0;
    pthread_rwlock_unlock(&pl->lock);
    pthread_rwlock_destroy(&pl->lock);       /* Destruye el candado */
}

/* ------------------------------- ESCRITURAS ------------------------------- */

int playlist_add(Playlist *pl, const char *path) {
    Track *node = malloc(sizeof(Track));     /* Reserva dinámica del nuevo nodo */
    if (!node) return -1;
    strncpy(node->path, path, PL_MAX_PATH - 1);
    node->path[PL_MAX_PATH - 1] = '\0';
    derive_name(path, node->name, PL_MAX_NAME);
    node->next = NULL;

    pthread_rwlock_wrlock(&pl->lock);        /* ESCRITURA: acceso exclusivo */
    node->prev = pl->tail;
    if (pl->tail) pl->tail->next = node;
    else          pl->head = node;           /* Lista estaba vacía */
    pl->tail = node;
    pl->count++;
    if (pl->current == NULL) pl->current = node; /* Primer elemento => también cursor */
    int idx = pl->count - 1;
    pthread_rwlock_unlock(&pl->lock);
    return idx;
}

int playlist_remove(Playlist *pl, int index) {
    pthread_rwlock_wrlock(&pl->lock);        /* ESCRITURA */
    Track *t = node_at(pl, index);
    if (!t) { pthread_rwlock_unlock(&pl->lock); return -1; }

    /* Si eliminamos el nodo apuntado por el cursor, movemos el cursor al siguiente
     * (o al anterior si era el último). Esto evita dejar 'current' colgando. */
    if (pl->current == t)
        pl->current = t->next ? t->next : t->prev;

    if (t->prev) t->prev->next = t->next;
    else         pl->head = t->next;
    if (t->next) t->next->prev = t->prev;
    else         pl->tail = t->prev;

    free(t);                                 /* Libera el nodo removido */
    pl->count--;
    pthread_rwlock_unlock(&pl->lock);
    return 0;
}

int playlist_move(Playlist *pl, int from, int to) {
    pthread_rwlock_wrlock(&pl->lock);        /* ESCRITURA */
    if (from < 0 || from >= pl->count || to < 0 || to >= pl->count || from == to) {
        pthread_rwlock_unlock(&pl->lock);
        return -1;
    }
    Track *node = node_at(pl, from);

    /* 1) Desenlazar el nodo de su posición actual */
    if (node->prev) node->prev->next = node->next;
    else            pl->head = node->next;
    if (node->next) node->next->prev = node->prev;
    else            pl->tail = node->prev;

    /* 2) Localizar el destino (recalculado tras desenlazar) e insertar antes de él.
     *    Si 'to' apunta al final, se inserta al final. */
    Track *dest = pl->head;
    int steps = (to > from) ? to : to;       /* índice destino en la lista ya desenlazada */
    for (int i = 0; i < steps && dest; i++) dest = dest->next;

    if (dest == NULL) {                      /* Insertar al final */
        node->prev = pl->tail;
        node->next = NULL;
        if (pl->tail) pl->tail->next = node;
        else          pl->head = node;
        pl->tail = node;
    } else {                                 /* Insertar antes de 'dest' */
        node->next = dest;
        node->prev = dest->prev;
        if (dest->prev) dest->prev->next = node;
        else            pl->head = node;
        dest->prev = node;
    }
    pthread_rwlock_unlock(&pl->lock);
    return 0;
}

void playlist_clear(Playlist *pl) {
    pthread_rwlock_wrlock(&pl->lock);        /* ESCRITURA */
    Track *t = pl->head;
    while (t) { Track *n = t->next; free(t); t = n; }
    pl->head = pl->tail = pl->current = NULL;
    pl->count = 0;
    pthread_rwlock_unlock(&pl->lock);
}

/* ------------------------- NAVEGACIÓN DEL CURSOR ------------------------- */

int playlist_select(Playlist *pl, int index) {
    pthread_rwlock_wrlock(&pl->lock);
    Track *t = node_at(pl, index);
    if (t) pl->current = t;
    int ok = (t != NULL) ? 1 : 0;
    pthread_rwlock_unlock(&pl->lock);
    return ok;
}

int playlist_next(Playlist *pl) {
    pthread_rwlock_wrlock(&pl->lock);
    int ok = 0;
    if (pl->current && pl->current->next) { pl->current = pl->current->next; ok = 1; }
    pthread_rwlock_unlock(&pl->lock);
    return ok;
}

int playlist_prev(Playlist *pl) {
    pthread_rwlock_wrlock(&pl->lock);
    int ok = 0;
    if (pl->current && pl->current->prev) { pl->current = pl->current->prev; ok = 1; }
    pthread_rwlock_unlock(&pl->lock);
    return ok;
}

/* -------------------------------- LECTURAS -------------------------------- */

int playlist_get_current(Playlist *pl, char *path_out, size_t path_sz,
                         char *name_out, size_t name_sz, int *index_out) {
    pthread_rwlock_rdlock(&pl->lock);        /* LECTURA: varios lectores en paralelo */
    int found = 0;
    if (pl->current) {
        /* Copiamos los datos para que el llamador NO retenga el puntero al nodo:
         * así, aunque un escritor lo elimine después, el llamador tiene una copia segura. */
        if (path_out) { strncpy(path_out, pl->current->path, path_sz - 1); path_out[path_sz - 1] = '\0'; }
        if (name_out) { strncpy(name_out, pl->current->name, name_sz - 1); name_out[name_sz - 1] = '\0'; }
        if (index_out) {
            int i = 0; for (Track *t = pl->head; t && t != pl->current; t = t->next) i++;
            *index_out = i;
        }
        found = 1;
    }
    pthread_rwlock_unlock(&pl->lock);
    return found;
}

int playlist_count(Playlist *pl) {
    pthread_rwlock_rdlock(&pl->lock);
    int c = pl->count;
    pthread_rwlock_unlock(&pl->lock);
    return c;
}

void playlist_print(Playlist *pl) {
    pthread_rwlock_rdlock(&pl->lock);        /* LECTURA */
    if (pl->count == 0) {
        printf("  \033[0;90m(cola vacía)\033[0m\n");
    } else {
        int i = 0;
        for (Track *t = pl->head; t; t = t->next, i++) {
            const char *marker = (t == pl->current) ? "\033[1;32m> \033[0m" : "  ";
            printf("%s[%d] %s\n", marker, i, t->name);
        }
    }
    pthread_rwlock_unlock(&pl->lock);
}
