#include "audio_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/stat.h>

/*
 * ====================================================================================
 * IMPLEMENTACIÓN DEL MOTOR DE AUDIO CONCURRENTE
 * ====================================================================================
 * Arquitectura de hilos (por pista):
 *   manager_thread  -> recorre la playlist; por cada pista crea producer + consumer,
 *                      espera a que terminen (join) y avanza/retrocede según el control.
 *   producer_thread -> open()+read() del MP3 -> rb_write() al búfer circular.
 *   consumer_thread -> rb_read() del búfer -> write() al pipe de ffplay (o sumidero).
 * ==================================================================================== */

#define CHUNK 4096                 /* Tamaño de bloque de I/O (bytes) */
#define RB_CAPACITY (256 * 1024)   /* Capacidad del búfer circular: 256 KB */
/* Tasa aproximada de consumo cuando NO hay audio real (para simular reproducción):
 * ~128 kbps MP3 => 16000 bytes/seg. Se usa solo en el modo sumidero temporizado. */
#define SIM_BYTES_PER_SEC 16000

/* Contexto que comparten productor y consumidor para la pista en curso. */
typedef struct {
    AudioEngine *e;
    int   fd;            /* FD del archivo de audio (productor) */
    int   audio_fd;      /* FD de escritura hacia ffplay, o -1 si sumidero */
    pid_t audio_pid;     /* PID de ffplay, o -1 */
} TrackCtx;

/* --------------------------- utilidades de control --------------------------- */

/* El consumidor llama a esto en cada iteración: si está en PAUSA, DUERME en la condvar
 * (sin espera activa) hasta que se reanude, se pida stop/next/prev o se salga. */
static void wait_if_paused(AudioEngine *e) {
    pthread_mutex_lock(&e->ctl_mutex);
    while (e->state == ST_PAUSED &&
           !e->request_stop && !e->request_next && !e->request_prev && !e->quit) {
        pthread_cond_wait(&e->ctl_cond, &e->ctl_mutex);
    }
    pthread_mutex_unlock(&e->ctl_mutex);
}

/* ¿Debe abortarse la pista actual? (stop/next/prev/quit) */
static int should_abort_track(AudioEngine *e) {
    pthread_mutex_lock(&e->ctl_mutex);
    int ab = e->request_stop || e->request_next || e->request_prev || e->quit;
    pthread_mutex_unlock(&e->ctl_mutex);
    return ab;
}

/* ------------------------------ salida de audio ------------------------------ */

/* Comprueba si ffplay está disponible en el sistema (una sola vez). */
static int ffplay_available(void) {
    return access("/usr/bin/ffplay", X_OK) == 0 || access("/bin/ffplay", X_OK) == 0;
}

/* Lanza ffplay como proceso hijo leyendo del pipe. Devuelve el FD de escritura del pipe
 * (donde el consumidor volcará el MP3) y el PID del hijo. Reutiliza el patrón del
 * profesor: fork() + dup2() del pipe a STDIN + exec* de ffplay. */
static int spawn_ffplay(pid_t *out_pid) {
    int pfd[2];
    if (pipe(pfd) == -1) return -1;          /* IPC: tubería hacia el reproductor */

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }

    if (pid == 0) {
        /* --- Proceso hijo: se convierte en ffplay --- */
        close(pfd[1]);                       /* No escribe */
        dup2(pfd[0], STDIN_FILENO);          /* El pipe pasa a ser su STDIN */
        close(pfd[0]);
        execlp("ffplay", "ffplay", "-i", "pipe:0", "-nodisp", "-autoexit",
               "-hide_banner", "-loglevel", "error", (char *)NULL);
        _exit(127);                          /* Solo si falla exec */
    }

    /* --- Proceso padre --- */
    close(pfd[0]);                           /* No lee */
    *out_pid = pid;
    return pfd[1];                           /* FD de escritura al reproductor */
}

/* ------------------------------- hilo productor ------------------------------- */

static void *producer_thread(void *arg) {
    TrackCtx *ctx = (TrackCtx *)arg;
    AudioEngine *e = ctx->e;
    unsigned char buf[CHUNK];
    ssize_t n;

    /* Lee del disco y empuja al búfer circular hasta EOF o abort. */
    while ((n = read(ctx->fd, buf, CHUNK)) > 0) {
        if (should_abort_track(e)) break;
        size_t w = rb_write(&e->rb, buf, (size_t)n);
        if (w == 0) break;                   /* shutdown del búfer */
    }
    rb_set_producer_done(&e->rb);            /* Avisa EOF al consumidor */
    return NULL;
}

/* ------------------------------- hilo consumidor ------------------------------ */

static void *consumer_thread(void *arg) {
    TrackCtx *ctx = (TrackCtx *)arg;
    AudioEngine *e = ctx->e;
    unsigned char buf[CHUNK];
    size_t r;

    while ((r = rb_read(&e->rb, buf, CHUNK)) > 0) {
        wait_if_paused(e);                   /* Respeta PAUSE sin consumir CPU */
        if (should_abort_track(e)) break;

        if (ctx->audio_fd >= 0) {
            /* Enviar al reproductor real; si el pipe se rompe (ffplay cerró), abortar. */
            ssize_t w = write(ctx->audio_fd, buf, r);
            if (w < 0) break;
        } else {
            /* Modo sumidero temporizado: no hay audio, simulamos el tiempo real de
             * reproducción durmiendo proporcional a los bytes "reproducidos". */
            struct timespec ts;
            double secs = (double)r / (double)SIM_BYTES_PER_SEC;
            ts.tv_sec  = (time_t)secs;
            ts.tv_nsec = (long)((secs - ts.tv_sec) * 1e9);
            nanosleep(&ts, NULL);
        }

        /* Actualizar progreso bajo el mutex de control. */
        pthread_mutex_lock(&e->ctl_mutex);
        e->cur_played += r;
        pthread_mutex_unlock(&e->ctl_mutex);
    }
    return NULL;
}

/* ----------------------- reproducción de UNA pista ----------------------- */

/* Reproduce la pista 'path'. Crea productor y consumidor, espera su término (join) y
 * limpia los recursos (FD de archivo, pipe y proceso ffplay). */
static void play_one_track(AudioEngine *e, const char *path, const char *name) {
    TrackCtx ctx;
    ctx.e = e;
    ctx.audio_fd = -1;
    ctx.audio_pid = -1;

    /* Abrir el archivo de audio con la syscall open() (no fopen). */
    ctx.fd = open(path, O_RDONLY);
    if (ctx.fd < 0) {
        fprintf(stderr, "\033[1;31m[motor] No se pudo abrir '%s': %s\033[0m\n",
                path, strerror(errno));
        return;
    }

    /* Tamaño total para el cálculo de progreso (fstat sobre el FD abierto). */
    struct stat st;
    size_t total = (fstat(ctx.fd, &st) == 0) ? (size_t)st.st_size : 0;

    /* Preparar la salida de audio. */
    if (e->audio_enabled) {
        ctx.audio_fd = spawn_ffplay(&ctx.audio_pid);
        if (ctx.audio_fd < 0) ctx.audio_fd = -1; /* fallback a sumidero */
    }

    /* Reset del búfer y de los contadores de la pista. */
    rb_reset(&e->rb);
    pthread_mutex_lock(&e->ctl_mutex);
    strncpy(e->cur_name, name, PL_MAX_NAME - 1);
    e->cur_name[PL_MAX_NAME - 1] = '\0';
    e->cur_total  = total;
    e->cur_played = 0;
    e->state = ST_PLAYING;
    e->request_stop = e->request_next = e->request_prev = 0;
    pthread_mutex_unlock(&e->ctl_mutex);

    printf("\n\033[1;36m[motor] Reproduciendo: %s %s\033[0m\n", name,
           e->audio_enabled && ctx.audio_fd >= 0 ? "" : "\033[0;90m(modo simulado, sin audio)\033[0m");

    /* Lanzar los dos hilos trabajadores. */
    pthread_t prod, cons;
    pthread_create(&prod, NULL, producer_thread, &ctx);
    pthread_create(&cons, NULL, consumer_thread, &ctx);

    /* Esperar a que ambos terminen (join limpio, sin dejar hilos sueltos). */
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    /* Limpieza de recursos de la pista. */
    close(ctx.fd);
    if (ctx.audio_fd >= 0) {
        close(ctx.audio_fd);                 /* Cerrar el pipe => ffplay recibe EOF */
        if (ctx.audio_pid > 0) waitpid(ctx.audio_pid, NULL, 0); /* Sin zombies */
    }
}

/* --------------------------------- hilo gestor --------------------------------- */

/* Recorre la playlist reproduciendo pista por pista. Entre pistas respeta las peticiones
 * de next/prev/stop. Si no hay pista o se pidió stop, espera (sin busy-wait) a un nuevo
 * play o a quit. */
static void *manager_thread(void *arg) {
    AudioEngine *e = (AudioEngine *)arg;

    while (1) {
        pthread_mutex_lock(&e->ctl_mutex);
        if (e->quit) { pthread_mutex_unlock(&e->ctl_mutex); break; }

        /* Si estamos detenidos, dormir hasta que haya un play/next/prev o quit. */
        while (e->state == ST_STOPPED && !e->quit)
            pthread_cond_wait(&e->ctl_cond, &e->ctl_mutex);

        if (e->quit) { pthread_mutex_unlock(&e->ctl_mutex); break; }
        pthread_mutex_unlock(&e->ctl_mutex);

        /* Obtener la pista actual de la playlist (lectura protegida por rwlock). */
        char path[PL_MAX_PATH], name[PL_MAX_NAME];
        int idx;
        if (!playlist_get_current(e->pl, path, sizeof(path), name, sizeof(name), &idx)) {
            printf("\033[0;90m[motor] Cola vacía. Agrega canciones con 'add'.\033[0m\n");
            pthread_mutex_lock(&e->ctl_mutex);
            e->state = ST_STOPPED;
            pthread_mutex_unlock(&e->ctl_mutex);
            continue;
        }

        play_one_track(e, path, name);       /* Reproduce (bloquea hasta terminar/abortar) */

        /* Decidir la siguiente acción tras finalizar la pista. */
        pthread_mutex_lock(&e->ctl_mutex);
        int do_next = e->request_next;
        int do_prev = e->request_prev;
        int do_stop = e->request_stop;
        int quit    = e->quit;
        e->request_next = e->request_prev = e->request_stop = 0;
        pthread_mutex_unlock(&e->ctl_mutex);

        if (quit) break;

        if (do_stop) {
            /* STOP: quedarse en la misma pista pero detenido. */
            pthread_mutex_lock(&e->ctl_mutex);
            e->state = ST_STOPPED;
            pthread_mutex_unlock(&e->ctl_mutex);
            continue;
        }

        if (do_prev) {
            /* PREV explícito: retroceder una pista y seguir reproduciendo. */
            playlist_prev(e->pl);
            continue;
        }

        /* Resto de casos (NEXT explícito o fin natural de la pista): avanzar.
         * do_next se consume aquí; si no hay siguiente, se detiene la reproducción. */
        (void)do_next;
        if (!playlist_next(e->pl)) {
            printf("\033[0;90m[motor] Fin de la lista de reproducción.\033[0m\n");
            pthread_mutex_lock(&e->ctl_mutex);
            e->state = ST_STOPPED;
            pthread_mutex_unlock(&e->ctl_mutex);
        }
    }
    return NULL;
}

/* ------------------------------- API pública ------------------------------- */

void engine_init(AudioEngine *e, Playlist *pl) {
    e->pl = pl;
    rb_init(&e->rb, RB_CAPACITY);
    e->state = ST_STOPPED;
    e->request_next = e->request_prev = e->request_stop = e->quit = 0;
    e->cur_name[0] = '\0';
    e->cur_total = e->cur_played = 0;
    e->manager_running = 0;
    e->audio_enabled = ffplay_available();
    pthread_mutex_init(&e->ctl_mutex, NULL);
    pthread_cond_init(&e->ctl_cond, NULL);
}

void engine_start(AudioEngine *e) {
    e->manager_running = 1;
    pthread_create(&e->manager_tid, NULL, manager_thread, e);
}

void engine_shutdown(AudioEngine *e) {
    /* Señalizar salida y abortar cualquier espera en todos los puntos. */
    pthread_mutex_lock(&e->ctl_mutex);
    e->quit = 1;
    e->request_stop = 1;
    pthread_cond_broadcast(&e->ctl_cond);    /* Despertar al gestor/consumidor */
    pthread_mutex_unlock(&e->ctl_mutex);
    rb_shutdown(&e->rb);                     /* Abortar esperas del búfer */

    if (e->manager_running) {
        pthread_join(e->manager_tid, NULL);  /* Join limpio del hilo gestor */
        e->manager_running = 0;
    }
    rb_destroy(&e->rb);
    pthread_mutex_destroy(&e->ctl_mutex);
    pthread_cond_destroy(&e->ctl_cond);
}

void engine_play(AudioEngine *e) {
    pthread_mutex_lock(&e->ctl_mutex);
    if (e->state == ST_PAUSED) e->state = ST_PLAYING;    /* Reanudar */
    else if (e->state == ST_STOPPED) e->state = ST_PLAYING; /* Arrancar */
    pthread_cond_broadcast(&e->ctl_cond);    /* Despertar gestor y consumidor */
    pthread_mutex_unlock(&e->ctl_mutex);
}

void engine_pause(AudioEngine *e) {
    pthread_mutex_lock(&e->ctl_mutex);
    if (e->state == ST_PLAYING) e->state = ST_PAUSED;
    pthread_cond_broadcast(&e->ctl_cond);
    pthread_mutex_unlock(&e->ctl_mutex);
}

void engine_toggle(AudioEngine *e) {
    pthread_mutex_lock(&e->ctl_mutex);
    if (e->state == ST_PLAYING)      e->state = ST_PAUSED;
    else if (e->state == ST_PAUSED)  e->state = ST_PLAYING;
    else                             e->state = ST_PLAYING;
    pthread_cond_broadcast(&e->ctl_cond);
    pthread_mutex_unlock(&e->ctl_mutex);
}

void engine_stop(AudioEngine *e) {
    pthread_mutex_lock(&e->ctl_mutex);
    e->request_stop = 1;
    e->state = ST_STOPPED;                   /* Reflejar DETENIDO de inmediato (status) */
    pthread_cond_broadcast(&e->ctl_cond);    /* Reanudar si estaba en pausa, para que aborte */
    pthread_mutex_unlock(&e->ctl_mutex);
    rb_shutdown(&e->rb);                     /* Desbloquear hilos de la pista actual */
    rb_reset(&e->rb);                        /* Dejar el búfer listo para la próxima */
}

void engine_next(AudioEngine *e) {
    pthread_mutex_lock(&e->ctl_mutex);
    e->request_next = 1;
    pthread_cond_broadcast(&e->ctl_cond);
    pthread_mutex_unlock(&e->ctl_mutex);
    rb_shutdown(&e->rb);
    rb_reset(&e->rb);
}

void engine_prev(AudioEngine *e) {
    pthread_mutex_lock(&e->ctl_mutex);
    e->request_prev = 1;
    pthread_cond_broadcast(&e->ctl_cond);
    pthread_mutex_unlock(&e->ctl_mutex);
    rb_shutdown(&e->rb);
    rb_reset(&e->rb);
}

void engine_print_status(AudioEngine *e) {
    pthread_mutex_lock(&e->ctl_mutex);
    const char *st = e->state == ST_PLAYING ? "REPRODUCIENDO" :
                     e->state == ST_PAUSED  ? "PAUSADO" : "DETENIDO";
    double pct = (e->cur_total > 0) ? (100.0 * e->cur_played / e->cur_total) : 0.0;
    printf("\033[1;36m[estado] %s", st);
    if (e->state != ST_STOPPED && e->cur_name[0]) {
        printf(" | %s | %.1f%% (%zu/%zu bytes)", e->cur_name, pct, e->cur_played, e->cur_total);
    }
    printf("%s\033[0m\n", e->audio_enabled ? "" : " | \033[0;90msin audio (simulado)\033[0m");
    pthread_mutex_unlock(&e->ctl_mutex);
}
