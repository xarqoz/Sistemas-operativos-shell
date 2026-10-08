#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

/*
 * ====================================================================================
 * MÓDULO: audio_engine (Motor de Reproducción Concurrente)
 * ====================================================================================
 * Orquesta la reproducción desacoplada mediante DOS hilos trabajadores por pista:
 *
 *   - HILO PRODUCTOR  (reader/decoder): abre el archivo con open() y lee bytes por
 *     chunks que deposita en el RingBuffer (rb_write).
 *   - HILO CONSUMIDOR (audio out): extrae bytes del RingBuffer (rb_read) y los envía al
 *     reproductor externo (ffplay) a través de un pipe; si no hay audio disponible,
 *     usa un sumidero temporizado para simular el consumo a la tasa de reproducción.
 *
 * El CONTROL de reproducción (play/pause/stop/next/prev) es ASÍNCRONO: el hilo de la CLI
 * cambia el estado y lo señaliza con una variable de condición, de modo que PAUSE no hace
 * espera activa (el consumidor duerme hasta que se reanuda).
 *
 * Sincronización usada explícitamente: pthread_t, pthread_mutex_t, pthread_cond_t
 * (en el estado de control) y el RingBuffer (mutex + condvars). La playlist se consulta
 * mediante su propio rwlock. No se usan librerías de alto nivel de sincronización.
 * ==================================================================================== */

#include <pthread.h>
#include "ring_buffer.h"
#include "playlist.h"

/* Estados de reproducción del motor. */
typedef enum {
    ST_STOPPED = 0,   /* Sin reproducir */
    ST_PLAYING,       /* Reproduciendo */
    ST_PAUSED         /* Pausado (el consumidor duerme en la condvar, sin consumir CPU) */
} PlayState;

typedef struct {
    Playlist  *pl;              /* Playlist compartida (no es propiedad del motor) */
    RingBuffer rb;              /* Búfer circular productor-consumidor por pista */

    /* --- Estado de control, protegido por su propio mutex --- */
    PlayState state;
    int       request_next;     /* El usuario pidió avanzar de pista */
    int       request_prev;     /* El usuario pidió retroceder de pista */
    int       request_stop;     /* El usuario pidió detener la pista actual */
    int       quit;             /* 1 => terminar el hilo gestor y todo el motor */
    int       audio_enabled;    /* 1 si hay reproductor externo disponible (ffplay) */

    char      cur_name[PL_MAX_NAME];  /* Nombre de la pista en curso (para el monitor) */
    size_t    cur_total;        /* Tamaño total en bytes de la pista actual */
    size_t    cur_played;       /* Bytes ya enviados a la salida (para el progreso) */

    pthread_mutex_t ctl_mutex;  /* Protege el estado de control de arriba */
    pthread_cond_t  ctl_cond;   /* Señaliza cambios de estado (reanudar desde pausa, etc.) */

    pthread_t manager_tid;      /* Hilo gestor que recorre la playlist y lanza pistas */
    int       manager_running;  /* 1 mientras el hilo gestor está activo */
} AudioEngine;

/* --- API pública (usada por la CLI) --- */
void engine_init(AudioEngine *e, Playlist *pl);
void engine_start(AudioEngine *e);   /* Lanza el hilo gestor */
void engine_shutdown(AudioEngine *e);/* Detiene todo y hace join limpio de los hilos */

/* Comandos de control asíncronos e inmediatos (no bloquean a la CLI). */
void engine_play(AudioEngine *e);
void engine_pause(AudioEngine *e);
void engine_toggle(AudioEngine *e);
void engine_stop(AudioEngine *e);
void engine_next(AudioEngine *e);
void engine_prev(AudioEngine *e);

/* Imprime el estado/progreso actual (para el comando 'status'). */
void engine_print_status(AudioEngine *e);

#endif /* AUDIO_ENGINE_H */
