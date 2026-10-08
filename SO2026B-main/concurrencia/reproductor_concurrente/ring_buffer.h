#ifndef RING_BUFFER_H
#define RING_BUFFER_H

/*
 * ====================================================================================
 * MÓDULO: ring_buffer (Búfer Circular Productor-Consumidor)
 * ====================================================================================
 * Búfer circular acotado (bounded buffer) sincronizado a mano con UN mutex y DOS
 * variables de condición. Es el corazón del desacoplamiento entre:
 *
 *   - PRODUCTOR: hilo lector/decodificador que lee bytes del archivo de audio y los
 *     deposita en el búfer.
 *   - CONSUMIDOR: hilo de salida de audio que extrae bytes y los envía al reproductor.
 *
 * Objetivos de sincronización (exigidos por la rúbrica):
 *   - Prevenir SOBRELLENADO: el productor espera (bloqueado, NO en espera activa) cuando
 *     el búfer está lleno, usando la condición 'not_full'.
 *   - Prevenir SUBDESBORDAMIENTO (underrun): el consumidor espera cuando el búfer está
 *     vacío, usando la condición 'not_empty'.
 *   - SIN busy-waiting: ambos hilos duermen en pthread_cond_wait y son despertados con
 *     pthread_cond_signal solo cuando hay un cambio real de estado.
 *
 * Se usan explícitamente pthread_mutex_t y pthread_cond_t (no librerías de alto nivel),
 * conforme a las instrucciones del parcial.
 * ==================================================================================== */

#include <pthread.h>
#include <stddef.h>

typedef struct {
    unsigned char *data;        /* Memoria del búfer (reservada con malloc) */
    size_t capacity;            /* Capacidad total en bytes */
    size_t head;                /* Índice de escritura (productor) */
    size_t tail;                /* Índice de lectura (consumidor) */
    size_t size;                /* Bytes actualmente ocupados */

    int producer_done;          /* 1 cuando el productor terminó de escribir (EOF) */
    int shutdown;               /* 1 para abortar inmediatamente (stop/salida) */

    pthread_mutex_t mutex;      /* Protege TODOS los campos de arriba */
    pthread_cond_t  not_full;   /* El productor espera aquí cuando el búfer está lleno */
    pthread_cond_t  not_empty;  /* El consumidor espera aquí cuando el búfer está vacío */
} RingBuffer;

/* --- Ciclo de vida --- */
int  rb_init(RingBuffer *rb, size_t capacity);
void rb_destroy(RingBuffer *rb);

/* Reinicia el contenido del búfer para una nueva pista (vacía datos y banderas),
 * conservando la memoria ya reservada. */
void rb_reset(RingBuffer *rb);

/* Productor: escribe 'len' bytes; bloquea si no hay espacio. Devuelve bytes escritos
 * (0 si se solicitó shutdown mientras esperaba). */
size_t rb_write(RingBuffer *rb, const unsigned char *buf, size_t len);

/* Consumidor: lee hasta 'len' bytes; bloquea si está vacío mientras el productor siga
 * activo. Devuelve bytes leídos (0 => fin de pista o shutdown). */
size_t rb_read(RingBuffer *rb, unsigned char *buf, size_t len);

/* Señaliza que el productor terminó (EOF): despierta al consumidor para que drene. */
void rb_set_producer_done(RingBuffer *rb);

/* Aborta todas las esperas (para stop/salida): despierta a ambos hilos. */
void rb_shutdown(RingBuffer *rb);

/* Lectura puntual del nivel de ocupación (para el monitor de progreso). */
size_t rb_level(RingBuffer *rb);

#endif /* RING_BUFFER_H */
