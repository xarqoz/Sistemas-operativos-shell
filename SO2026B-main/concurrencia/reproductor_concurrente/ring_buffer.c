#include "ring_buffer.h"
#include <stdlib.h>
#include <string.h>

/*
 * ====================================================================================
 * IMPLEMENTACIÓN DEL BÚFER CIRCULAR SINCRONIZADO
 * ====================================================================================
 * Patrón productor-consumidor con bounded buffer. La invariante protegida por el mutex
 * es: 0 <= size <= capacity, con 'head' y 'tail' avanzando en módulo 'capacity'.
 * ==================================================================================== */

int rb_init(RingBuffer *rb, size_t capacity) {
    rb->data = malloc(capacity);             /* Memoria dinámica del búfer */
    if (!rb->data) return -1;
    rb->capacity = capacity;
    rb->head = rb->tail = rb->size = 0;
    rb->producer_done = 0;
    rb->shutdown = 0;
    pthread_mutex_init(&rb->mutex, NULL);
    pthread_cond_init(&rb->not_full, NULL);
    pthread_cond_init(&rb->not_empty, NULL);
    return 0;
}

void rb_destroy(RingBuffer *rb) {
    free(rb->data);                          /* Libera la memoria del búfer */
    rb->data = NULL;
    pthread_mutex_destroy(&rb->mutex);
    pthread_cond_destroy(&rb->not_full);
    pthread_cond_destroy(&rb->not_empty);
}

void rb_reset(RingBuffer *rb) {
    pthread_mutex_lock(&rb->mutex);
    rb->head = rb->tail = rb->size = 0;
    rb->producer_done = 0;
    rb->shutdown = 0;
    pthread_mutex_unlock(&rb->mutex);
}

size_t rb_write(RingBuffer *rb, const unsigned char *buf, size_t len) {
    size_t written = 0;
    pthread_mutex_lock(&rb->mutex);          /* Entrar a la sección crítica */
    while (written < len) {
        /* Si el búfer está lleno, el productor DUERME en 'not_full' (sin espera activa)
         * hasta que el consumidor libere espacio o se pida shutdown. */
        while (rb->size == rb->capacity && !rb->shutdown)
            pthread_cond_wait(&rb->not_full, &rb->mutex);

        if (rb->shutdown) break;             /* Abortar: salir con lo escrito hasta ahora */

        /* Copiar tantos bytes como quepan de forma contigua hasta el final del arreglo */
        size_t space = rb->capacity - rb->size;
        size_t chunk = len - written;
        if (chunk > space) chunk = space;
        size_t to_end = rb->capacity - rb->head;
        if (chunk > to_end) chunk = to_end;  /* No pasar del borde (envoltura circular) */

        memcpy(rb->data + rb->head, buf + written, chunk);
        rb->head = (rb->head + chunk) % rb->capacity;
        rb->size += chunk;
        written += chunk;

        /* Hay datos nuevos: despertar a un consumidor que pudiera estar esperando. */
        pthread_cond_signal(&rb->not_empty);
    }
    pthread_mutex_unlock(&rb->mutex);
    return written;
}

size_t rb_read(RingBuffer *rb, unsigned char *buf, size_t len) {
    pthread_mutex_lock(&rb->mutex);
    /* Si está vacío pero el productor sigue activo, el consumidor DUERME en 'not_empty'
     * (previene underrun por espera activa). Si el productor ya terminó y no hay datos,
     * devolvemos 0 (fin de pista). */
    while (rb->size == 0 && !rb->producer_done && !rb->shutdown)
        pthread_cond_wait(&rb->not_empty, &rb->mutex);

    if (rb->shutdown || (rb->size == 0 && rb->producer_done)) {
        pthread_mutex_unlock(&rb->mutex);
        return 0;                            /* Nada más que leer */
    }

    /* Leer tantos bytes como haya disponibles de forma contigua */
    size_t avail = rb->size;
    size_t chunk = (len < avail) ? len : avail;
    size_t to_end = rb->capacity - rb->tail;
    if (chunk > to_end) chunk = to_end;      /* Respetar la envoltura circular */

    memcpy(buf, rb->data + rb->tail, chunk);
    rb->tail = (rb->tail + chunk) % rb->capacity;
    rb->size -= chunk;

    /* Se liberó espacio: despertar al productor que pudiera estar bloqueado. */
    pthread_cond_signal(&rb->not_full);
    pthread_mutex_unlock(&rb->mutex);
    return chunk;
}

void rb_set_producer_done(RingBuffer *rb) {
    pthread_mutex_lock(&rb->mutex);
    rb->producer_done = 1;
    /* Despertar al consumidor por si está esperando datos que ya no llegarán. */
    pthread_cond_broadcast(&rb->not_empty);
    pthread_mutex_unlock(&rb->mutex);
}

void rb_shutdown(RingBuffer *rb) {
    pthread_mutex_lock(&rb->mutex);
    rb->shutdown = 1;
    /* Despertar a AMBOS hilos para que salgan de sus esperas de inmediato. */
    pthread_cond_broadcast(&rb->not_full);
    pthread_cond_broadcast(&rb->not_empty);
    pthread_mutex_unlock(&rb->mutex);
}

size_t rb_level(RingBuffer *rb) {
    pthread_mutex_lock(&rb->mutex);
    size_t s = rb->size;
    pthread_mutex_unlock(&rb->mutex);
    return s;
}
