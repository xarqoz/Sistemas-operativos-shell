#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "playlist.h"
#include "audio_engine.h"

/*
 * ====================================================================================
 * REPRODUCTOR DE AUDIO CONCURRENTE (CLI) — Parcial 2, Alternativa 2
 * ====================================================================================
 * Hilo PRINCIPAL / UI: lee comandos del usuario desde STDIN (no bloquea a los hilos del
 * motor) y traduce cada comando a una llamada ASÍNCRONA del motor (play/pause/...) o a
 * una operación sobre la playlist. Mientras el usuario escribe comandos, los hilos
 * productor y consumidor siguen reproduciendo de forma concurrente.
 *
 * Separación de responsabilidades (diseño modular):
 *   - main.c          : interfaz CLI y traducción de comandos (esta capa).
 *   - audio_engine.*  : motor concurrente (hilos productor/consumidor + control).
 *   - ring_buffer.*   : búfer circular sincronizado (productor-consumidor).
 *   - playlist.*      : cola de reproducción concurrente (lectores-escritores).
 * ==================================================================================== */

static volatile sig_atomic_t g_interrupted = 0;

/* Manejo de Ctrl+C: en vez de matar el proceso de golpe, marcamos una bandera para
 * salir ordenadamente (join de hilos y liberación de recursos). */
static void on_sigint(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static void print_banner(const AudioEngine *e) {
    printf("\033[1;97m===================================================\033[0m\n");
    printf("\033[1;97m  Reproductor de Audio Concurrente (SO2026B - P2)\033[0m\n");
    printf("\033[0;90m  Alternativa 2: Playlist concurrente + búfer P/C\033[0m\n");
    if (!e->audio_enabled)
        printf("\033[0;90m  (ffplay no detectado: salida de audio SIMULADA)\033[0m\n");
    printf("\033[1;97m===================================================\033[0m\n");
    printf("Escribe 'help' para ver los comandos.\n\n");
}

static void print_help(void) {
    printf("\n\033[1;97m--- Comandos ---\033[0m\n");
    printf("  \033[1;36madd <ruta>\033[0m        Agrega una canción a la cola.\n");
    printf("  \033[1;36mrm <i>\033[0m            Elimina la canción en el índice i.\n");
    printf("  \033[1;36mmove <i> <j>\033[0m      Reordena: mueve la pista i a la posición j.\n");
    printf("  \033[1;36mlist\033[0m              Muestra la cola (marca la pista actual con >).\n");
    printf("  \033[1;36mclear\033[0m             Vacía la cola.\n");
    printf("  \033[1;36msel <i>\033[0m           Selecciona (cursor) la pista i.\n");
    printf("  \033[1;36mplay\033[0m              Reproduce / reanuda.\n");
    printf("  \033[1;36mpause\033[0m             Pausa.\n");
    printf("  \033[1;36mpp\033[0m                Alterna play/pausa.\n");
    printf("  \033[1;36mstop\033[0m              Detiene la reproducción.\n");
    printf("  \033[1;36mnext\033[0m              Salta a la siguiente pista.\n");
    printf("  \033[1;36mprev\033[0m              Vuelve a la pista anterior.\n");
    printf("  \033[1;36mstatus\033[0m            Muestra estado y progreso.\n");
    printf("  \033[1;36mhelp\033[0m              Esta ayuda.\n");
    printf("  \033[1;36mquit\033[0m / \033[1;36mexit\033[0m       Sale ordenadamente.\n\n");
}

int main(int argc, char **argv) {
    Playlist pl;
    AudioEngine engine;

    playlist_init(&pl);
    engine_init(&engine, &pl);

    /* Instalar manejador de SIGINT para salida limpia. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);

    /* Canciones pasadas como argumentos se agregan a la cola de entrada. */
    for (int i = 1; i < argc; i++) {
        if (playlist_add(&pl, argv[i]) >= 0)
            printf("\033[0;90mAgregado: %s\033[0m\n", argv[i]);
    }

    print_banner(&engine);
    engine_start(&engine);                   /* Arranca el hilo gestor del motor */

    char line[2048];
    while (!g_interrupted) {
        printf("\033[1;35mplayer>\033[0m ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) break; /* EOF (Ctrl+D) */

        /* Tokenizar: comando + hasta 2 argumentos (el 1º puede llevar espacios en 'add'). */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] == '\0') continue;

        char cmd[32] = {0};
        char arg1[1024] = {0};
        char arg2[1024] = {0};
        /* 'add' toma TODO el resto como ruta (puede contener espacios). */
        if (sscanf(line, "%31s", cmd) != 1) continue;

        if (strcmp(cmd, "add") == 0) {
            const char *rest = line + 3;
            while (*rest == ' ') rest++;
            if (*rest == '\0') { printf("Uso: add <ruta>\n"); continue; }
            int idx = playlist_add(&pl, rest);
            if (idx >= 0) printf("\033[1;32mAgregado en [%d]: %s\033[0m\n", idx, rest);
            else printf("\033[1;31mError al agregar.\033[0m\n");
        } else if (strcmp(cmd, "rm") == 0) {
            if (sscanf(line, "%*s %1023s", arg1) == 1) {
                if (playlist_remove(&pl, atoi(arg1)) == 0) printf("Eliminada [%s].\n", arg1);
                else printf("\033[1;31mÍndice inválido.\033[0m\n");
            } else printf("Uso: rm <i>\n");
        } else if (strcmp(cmd, "move") == 0) {
            if (sscanf(line, "%*s %1023s %1023s", arg1, arg2) == 2) {
                if (playlist_move(&pl, atoi(arg1), atoi(arg2)) == 0) printf("Movida %s -> %s.\n", arg1, arg2);
                else printf("\033[1;31mÍndices inválidos.\033[0m\n");
            } else printf("Uso: move <i> <j>\n");
        } else if (strcmp(cmd, "list") == 0) {
            printf("\033[1;97m--- Cola (%d) ---\033[0m\n", playlist_count(&pl));
            playlist_print(&pl);
        } else if (strcmp(cmd, "clear") == 0) {
            engine_stop(&engine);
            playlist_clear(&pl);
            printf("Cola vaciada.\n");
        } else if (strcmp(cmd, "sel") == 0) {
            if (sscanf(line, "%*s %1023s", arg1) == 1) {
                if (playlist_select(&pl, atoi(arg1))) printf("Seleccionada [%s].\n", arg1);
                else printf("\033[1;31mÍndice inválido.\033[0m\n");
            } else printf("Uso: sel <i>\n");
        } else if (strcmp(cmd, "play") == 0) {
            engine_play(&engine);
        } else if (strcmp(cmd, "pause") == 0) {
            engine_pause(&engine);
        } else if (strcmp(cmd, "pp") == 0) {
            engine_toggle(&engine);
        } else if (strcmp(cmd, "stop") == 0) {
            engine_stop(&engine);
        } else if (strcmp(cmd, "next") == 0) {
            engine_next(&engine);
        } else if (strcmp(cmd, "prev") == 0) {
            engine_prev(&engine);
        } else if (strcmp(cmd, "status") == 0) {
            engine_print_status(&engine);
        } else if (strcmp(cmd, "help") == 0) {
            print_help();
        } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            break;
        } else {
            printf("\033[1;31mComando desconocido: '%s' (usa 'help').\033[0m\n", cmd);
        }
    }

    printf("\n\033[0;90mCerrando ordenadamente...\033[0m\n");
    engine_shutdown(&engine);                /* Join de hilos + liberación del motor */
    playlist_destroy(&pl);                   /* Libera todos los nodos y el rwlock */
    printf("\033[1;32mAdiós.\033[0m\n");
    return 0;
}
