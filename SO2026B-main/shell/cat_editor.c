#include "shell.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>

/**
 * ====================================================================================
 * CATEGORÍA: editor  (cat_editor.c)
 * ====================================================================================
 * Editor de texto interactivo operado estrictamente por CLI (inspirado en `ed`/`vi`),
 * integrado al Shell Educativo de Syscalls (SO2026B).
 *
 * DECISIÓN ARQUITECTÓNICA (pensamiento crítico):
 * ----------------------------------------------
 * El resto de comandos del shell (datos, memoria, monitoreo, utilidades) son comandos
 * ATÓMICOS: se ejecutan una vez, demuestran una o pocas syscalls y retornan de inmediato
 * al prompt principal (REPL). El editor, en cambio, es una APLICACIÓN INTERACTIVA con
 * estado persistente (un File Descriptor abierto, un portapapeles, pilas de undo/redo y
 * un archivo swap temporal). Forzarlo dentro de la categoría "datos" rompería esa
 * semántica atómica y mezclaría conceptos.
 *
 * Por ello se JUSTIFICA la creación de una CATEGORÍA NUEVA llamada "editor": el comando
 * `editor [archivo]` actúa como punto de entrada que arranca un SUB-REPL propio
 * (ciclo Read-Eval-Print anidado). Mientras dura la sesión del editor, el shell cede el
 * control al bucle del editor; al salir con `q` se devuelve el control al prompt
 * principal del shell. Esto respeta la arquitectura de enrutamiento por tabla de
 * comandos sin contaminar las categorías existentes.
 *
 * RESTRICCIÓN CRÍTICA DE I/O (cumplida):
 * --------------------------------------
 * Todo acceso al archivo de texto en disco se hace EXCLUSIVAMENTE con llamadas al
 * sistema de bajo nivel: open(), read(), write(), lseek(), ftruncate(), close(),
 * fstat() y unlink(). NO se usan fopen/fread/fwrite/fclose. La I/O estándar (printf,
 * fgets) se emplea ÚNICAMENTE para leer los comandos del usuario desde STDIN y para
 * imprimir en consola (STDOUT).
 *
 * ESCALAMIENTO POR TAMAÑO DE EQUIPO (acumulativo, equipo de 4 integrantes):
 *   - Base (Sección 3):  o, p, a, d, q
 *   - Equipo 2 (Parejas): i (inserción arbitraria), s (búsqueda simple)
 *   - Equipo 3 (Tres):    m (metadatos con fstat), y (copiar línea), x (cortar línea)
 *   - Equipo 4 (Cuatro):  u (undo), r (redo) usando archivos swap en /tmp + unlink()
 * ====================================================================================
 */

#define ED_MAX_LINE   4096   /* Tamaño máximo de una línea de comando del editor */
#define ED_CHUNK      4096   /* Tamaño del búfer de lectura/desplazamiento de bytes */
#define ED_UNDO_MAX   64     /* Profundidad máxima de las pilas de undo/redo */

/**
 * Estado completo de una sesión del editor.
 * Se mantiene en la pila (stack) de cmd_editor; no se usa estado global para evitar
 * fugas o condiciones residuales entre invocaciones.
 */
typedef struct {
    int   fd;                 /* File Descriptor del archivo abierto (open con O_RDWR|O_CREAT) */
    char  filename[1024];     /* Nombre del archivo de trabajo */

    char *clipboard;          /* Portapapeles secuencial local (malloc/free) para y/x */
    size_t clipboard_len;     /* Longitud en bytes del portapapeles */

    /* Pilas de Undo/Redo basadas en snapshots guardados como archivos swap en /tmp */
    char undo_stack[ED_UNDO_MAX][64];  /* Rutas de los archivos swap de undo */
    int  undo_top;                     /* Índice de tope de la pila de undo */
    char redo_stack[ED_UNDO_MAX][64];  /* Rutas de los archivos swap de redo */
    int  redo_top;                     /* Índice de tope de la pila de redo */
    unsigned long swap_counter;        /* Contador para nombres únicos de swap */
} EditorState;

/* ====================================================================================
 * UTILIDADES INTERNAS DE BAJO NIVEL (todas basadas en syscalls)
 * ==================================================================================== */

/**
 * Lee TODO el contenido del archivo abierto (ed->fd) a un búfer dinámico en memoria.
 * Devuelve el puntero al búfer (que el llamador debe liberar con free) y escribe en
 * *out_len la cantidad de bytes leídos. Devuelve NULL en error.
 *
 * Syscalls: lseek(2) (para posicionar), read(2).
 */
static char *ed_read_all(EditorState *ed, size_t *out_len) {
    /* Posicionar al inicio del archivo */
    if (lseek(ed->fd, 0, SEEK_SET) == (off_t)-1) {
        LOG_SYSCALL("lseek", "%d, 0, SEEK_SET", ed->fd);
        LOG_SYSCALL_ERROR(strerror(errno));
        return NULL;
    }

    size_t cap = ED_CHUNK;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    char chunk[ED_CHUNK];
    ssize_t n;
    while ((n = read(ed->fd, chunk, sizeof(chunk))) > 0) {
        if (len + (size_t)n + 1 > cap) {
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        memcpy(buf + len, chunk, (size_t)n);
        len += (size_t)n;
    }
    if (n == -1) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/**
 * Reescribe COMPLETAMENTE el archivo en disco con el contenido del búfer dado.
 * Trunca el archivo al nuevo tamaño exacto para no dejar bytes residuales.
 *
 * Syscalls: lseek(2) (SEEK_SET), write(2), ftruncate(2).
 */
static int ed_write_all(EditorState *ed, const char *data, size_t len) {
    if (lseek(ed->fd, 0, SEEK_SET) == (off_t)-1) {
        LOG_SYSCALL("lseek", "%d, 0, SEEK_SET", ed->fd);
        LOG_SYSCALL_ERROR(strerror(errno));
        return -1;
    }

    size_t written = 0;
    while (written < len) {
        ssize_t w = write(ed->fd, data + written, len - written);
        if (w == -1) {
            LOG_SYSCALL("write", "%d, buffer, %zu", ed->fd, len - written);
            LOG_SYSCALL_ERROR(strerror(errno));
            return -1;
        }
        written += (size_t)w;
    }

    /* Truncar al tamaño exacto para eliminar restos de contenido anterior */
    LOG_SYSCALL("ftruncate", "%d, %zu", ed->fd, len);
    if (ftruncate(ed->fd, (off_t)len) == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return -1;
    }
    LOG_SYSCALL_RESULT(0);
    return 0;
}

/**
 * Devuelve los límites [inicio, fin) en bytes de la línea número `n` (1-indexada)
 * dentro de `buf`. `fin` apunta al byte siguiente al '\n' (o al final del búfer).
 * Devuelve 1 si la línea existe, 0 si no. También reporta el total de líneas.
 */
static int ed_line_bounds(const char *buf, size_t len, int n,
                          size_t *start, size_t *end, int *total_lines) {
    int line = 1;
    size_t i = 0;
    size_t ln_start = 0;
    int found = 0;
    *start = 0; *end = 0;

    if (n >= 1) {
        for (i = 0; i < len; i++) {
            if (buf[i] == '\n') {
                if (line == n) {
                    *start = ln_start;
                    *end = i + 1; /* incluir el salto de línea */
                    found = 1;
                }
                line++;
                ln_start = i + 1;
            }
        }
        /* Última línea sin '\n' final */
        if (!found && ln_start < len && line == n) {
            *start = ln_start;
            *end = len;
            found = 1;
        }
    }

    /* Calcular total de líneas */
    int tl = 0;
    if (len > 0) {
        tl = 1;
        for (i = 0; i < len; i++) {
            if (buf[i] == '\n' && i + 1 < len) tl++;
            else if (buf[i] == '\n' && i + 1 == len) { /* '\n' final no suma línea extra */ }
        }
        /* Si el archivo termina en '\n', la cuenta anterior ya es correcta */
    }
    if (total_lines) *total_lines = tl;
    return found;
}

/* ====================================================================================
 * SOPORTE DE UNDO / REDO (Equipo de 4): archivos swap en /tmp + unlink()
 * ====================================================================================
 * Antes de cada operación destructiva se toma un "snapshot" del contenido actual y se
 * guarda en un archivo temporal en /tmp usando open()/write()/close(). La ruta se apila
 * en undo_stack. Al hacer `u` (undo) se restaura ese snapshot y la ruta se mueve a
 * redo_stack. Al salir (`q`) todos los swaps se eliminan con unlink() para no dejar
 * basura en el sistema de ficheros.
 */

/* Crea un archivo swap en /tmp con el contenido dado. Devuelve 0 y escribe la ruta. */
static int ed_make_swap(EditorState *ed, const char *data, size_t len, char *out_path) {
    snprintf(out_path, 64, "/tmp/eafitos_edit_%d_%lu.swap",
             (int)getpid(), ed->swap_counter++);

    LOG_SYSCALL("open", "\"%s\", O_WRONLY|O_CREAT|O_TRUNC, 0600", out_path);
    int sfd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (sfd == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return -1;
    }
    LOG_SYSCALL_RESULT(sfd);

    size_t written = 0;
    while (written < len) {
        ssize_t w = write(sfd, data + written, len - written);
        if (w == -1) { close(sfd); return -1; }
        written += (size_t)w;
    }

    LOG_SYSCALL("close", "%d", sfd);
    close(sfd);
    LOG_SYSCALL_RESULT(0);
    return 0;
}

/* Lee completamente un archivo swap de /tmp. Devuelve búfer (free por el llamador). */
static char *ed_read_swap(const char *path, size_t *out_len) {
    LOG_SYSCALL("open", "\"%s\", O_RDONLY", path);
    int sfd = open(path, O_RDONLY);
    if (sfd == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return NULL;
    }
    LOG_SYSCALL_RESULT(sfd);

    size_t cap = ED_CHUNK, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(sfd); return NULL; }

    char chunk[ED_CHUNK];
    ssize_t n;
    while ((n = read(sfd, chunk, sizeof(chunk))) > 0) {
        if (len + (size_t)n + 1 > cap) {
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(sfd); return NULL; }
            buf = nb;
        }
        memcpy(buf + len, chunk, (size_t)n);
        len += (size_t)n;
    }
    close(sfd);
    if (n == -1) { free(buf); return NULL; }
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* Toma un snapshot del estado actual y lo apila en undo_stack. Limpia la pila de redo. */
static void ed_push_undo(EditorState *ed) {
    size_t len = 0;
    char *cur = ed_read_all(ed, &len);
    if (!cur) return;

    if (ed->undo_top >= ED_UNDO_MAX) {
        /* Pila llena: descartar el snapshot más antiguo liberando su swap */
        LOG_SYSCALL("unlink", "\"%s\"", ed->undo_stack[0]);
        unlink(ed->undo_stack[0]);
        LOG_SYSCALL_RESULT(0);
        for (int i = 1; i < ED_UNDO_MAX; i++)
            memcpy(ed->undo_stack[i - 1], ed->undo_stack[i], 64);
        ed->undo_top--;
    }

    if (ed_make_swap(ed, cur, len, ed->undo_stack[ed->undo_top]) == 0)
        ed->undo_top++;
    free(cur);

    /* Toda nueva edición invalida el historial de redo */
    while (ed->redo_top > 0) {
        ed->redo_top--;
        LOG_SYSCALL("unlink", "\"%s\"", ed->redo_stack[ed->redo_top]);
        unlink(ed->redo_stack[ed->redo_top]);
        LOG_SYSCALL_RESULT(0);
    }
}

/* ====================================================================================
 * SUBCOMANDOS DEL EDITOR
 * ==================================================================================== */

/* --- Base: p [n]  -> imprime línea n, o todo el archivo si no hay n --- */
static void ed_cmd_print(EditorState *ed, const char *arg) {
    size_t len = 0;
    char *buf = ed_read_all(ed, &len);
    if (!buf) return;

    if (arg == NULL) {
        /* Imprimir todo el archivo byte a byte en STDOUT (FD 1) vía write */
        LOG_SYSCALL("write", "1, buffer, %zu", len);
        if (len > 0) write(1, buf, len);
        LOG_SYSCALL_RESULT(len);
        if (len > 0 && buf[len - 1] != '\n') printf("\n");
    } else {
        int n = atoi(arg);
        size_t s, e; int total = 0;
        if (ed_line_bounds(buf, len, n, &s, &e, &total)) {
            LOG_SYSCALL("write", "1, &buffer[%zu], %zu", s, e - s);
            write(1, buf + s, e - s);
            LOG_SYSCALL_RESULT(e - s);
            if (e == len && (e == 0 || buf[e - 1] != '\n')) printf("\n");
        } else {
            printf(COLOR_ERROR "La línea %d no existe (el archivo tiene %d línea(s)).\n" COLOR_RESET, n, total);
        }
    }
    free(buf);
}

/* --- Base: a [texto]  -> añade texto como nueva línea al final --- */
static void ed_cmd_append(EditorState *ed, const char *text) {
    if (text == NULL) text = "";
    ed_push_undo(ed);

    /* Posicionar al final del archivo */
    LOG_SYSCALL("lseek", "%d, 0, SEEK_END", ed->fd);
    off_t end = lseek(ed->fd, 0, SEEK_END);
    if (end == (off_t)-1) { LOG_SYSCALL_ERROR(strerror(errno)); return; }
    LOG_SYSCALL_RESULT((long)end);

    /* Si el archivo no está vacío y no termina en '\n', agregar uno antes */
    if (end > 0) {
        char last;
        lseek(ed->fd, end - 1, SEEK_SET);
        if (read(ed->fd, &last, 1) == 1 && last != '\n') {
            lseek(ed->fd, 0, SEEK_END);
            write(ed->fd, "\n", 1);
        }
        lseek(ed->fd, 0, SEEK_END);
    }

    size_t tlen = strlen(text);
    LOG_SYSCALL("write", "%d, \"%s\\n\", %zu", ed->fd, text, tlen + 1);
    if (write(ed->fd, text, tlen) == -1 || write(ed->fd, "\n", 1) == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return;
    }
    LOG_SYSCALL_RESULT(tlen + 1);
    printf(COLOR_RESULT "Línea añadida al final.\n" COLOR_RESET);
}

/* --- Base: d [n]  -> borra la línea n, desplazando bytes y truncando --- */
static void ed_cmd_delete(EditorState *ed, const char *arg) {
    if (arg == NULL) {
        printf(COLOR_ERROR "Uso: d <n>\n" COLOR_RESET);
        return;
    }
    int n = atoi(arg);
    size_t len = 0;
    char *buf = ed_read_all(ed, &len);
    if (!buf) return;

    size_t s, e; int total = 0;
    if (!ed_line_bounds(buf, len, n, &s, &e, &total)) {
        printf(COLOR_ERROR "La línea %d no existe (el archivo tiene %d línea(s)).\n" COLOR_RESET, n, total);
        free(buf);
        return;
    }

    ed_push_undo(ed);

    /* Desplazar los bytes posteriores sobre el hueco de la línea borrada */
    size_t new_len = len - (e - s);
    memmove(buf + s, buf + e, len - e);

    if (ed_write_all(ed, buf, new_len) == 0)
        printf(COLOR_RESULT "Línea %d borrada.\n" COLOR_RESET, n);
    free(buf);
}

/* --- Equipo 2: i [n] [texto]  -> inserta texto como nueva línea en la posición n --- */
static void ed_cmd_insert(EditorState *ed, const char *arg_n, const char *text) {
    if (arg_n == NULL) {
        printf(COLOR_ERROR "Uso: i <n> <texto>\n" COLOR_RESET);
        return;
    }
    if (text == NULL) text = "";
    int n = atoi(arg_n);

    size_t len = 0;
    char *buf = ed_read_all(ed, &len);
    if (!buf) return;

    size_t s, e; int total = 0;
    ed_line_bounds(buf, len, n, &s, &e, &total);

    /* Punto de inserción: inicio de la línea n (o final del archivo si n > total) */
    size_t pos;
    if (n <= 1) pos = 0;
    else if (n > total) pos = len;
    else pos = s;

    size_t tlen = strlen(text);
    size_t ins_len = tlen + 1; /* texto + '\n' */

    /* Buffer dinámico nuevo con el texto insertado (malloc/memmove) */
    char *out = malloc(len + ins_len + 1);
    if (!out) { free(buf); return; }

    ed_push_undo(ed);

    memcpy(out, buf, pos);
    memcpy(out + pos, text, tlen);
    out[pos + tlen] = '\n';
    memcpy(out + pos + ins_len, buf + pos, len - pos);
    size_t new_len = len + ins_len;

    if (ed_write_all(ed, out, new_len) == 0)
        printf(COLOR_RESULT "Texto insertado en la línea %d.\n" COLOR_RESET, n);

    free(out);
    free(buf);
}

/* --- Equipo 2: s [palabra]  -> búsqueda simple, lista líneas con coincidencia --- */
static void ed_cmd_search(EditorState *ed, const char *word) {
    if (word == NULL) {
        printf(COLOR_ERROR "Uso: s <palabra>\n" COLOR_RESET);
        return;
    }
    size_t len = 0;
    char *buf = ed_read_all(ed, &len);
    if (!buf) return;

    int line = 1, matches = 0;
    size_t ln_start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || buf[i] == '\n') {
            size_t ln_len = i - ln_start;
            char saved = buf[ln_start + ln_len];
            buf[ln_start + ln_len] = '\0';
            if (strstr(buf + ln_start, word) != NULL) {
                printf(COLOR_PARAM "L%d:" COLOR_RESET " %s\n", line, buf + ln_start);
                matches++;
            }
            buf[ln_start + ln_len] = saved;
            line++;
            ln_start = i + 1;
        }
    }
    if (matches == 0)
        printf(COLOR_INFO "Sin coincidencias para \"%s\".\n" COLOR_RESET, word);
    else
        printf(COLOR_RESULT "%d coincidencia(s) encontrada(s).\n" COLOR_RESET, matches);
    free(buf);
}

/* --- Equipo 3: m  -> metadatos del archivo (tamaño, permisos, inodo, modificación) --- */
static void ed_cmd_meta(EditorState *ed) {
    struct stat st;
    LOG_SYSCALL("fstat", "%d, &st", ed->fd);
    if (fstat(ed->fd, &st) == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return;
    }
    LOG_SYSCALL_RESULT(0);

    printf(COLOR_TITLE "--- Metadatos del Archivo (fstat) ---\n" COLOR_RESET);
    printf("  Archivo:        %s\n", ed->filename);
    printf("  Tamaño:         " COLOR_RESULT "%ld bytes" COLOR_RESET "\n", (long)st.st_size);
    printf("  Permisos (oct): " COLOR_PARAM "%o" COLOR_RESET "\n", st.st_mode & 0777);
    printf("  Inodo:          " COLOR_PARAM "%ld" COLOR_RESET "\n", (long)st.st_ino);
    printf("  MTime (Modif):  %s", ctime(&st.st_mtime));
    printf(COLOR_TITLE "-------------------------------------\n" COLOR_RESET);
}

/* --- Equipo 3: y [n]  -> copiar (yank) la línea n al portapapeles local --- */
static void ed_cmd_yank(EditorState *ed, const char *arg) {
    if (arg == NULL) {
        printf(COLOR_ERROR "Uso: y <n>\n" COLOR_RESET);
        return;
    }
    int n = atoi(arg);
    size_t len = 0;
    char *buf = ed_read_all(ed, &len);
    if (!buf) return;

    size_t s, e; int total = 0;
    if (!ed_line_bounds(buf, len, n, &s, &e, &total)) {
        printf(COLOR_ERROR "La línea %d no existe (el archivo tiene %d línea(s)).\n" COLOR_RESET, n, total);
        free(buf);
        return;
    }

    free(ed->clipboard);
    ed->clipboard_len = e - s;
    ed->clipboard = malloc(ed->clipboard_len + 1);
    if (ed->clipboard) {
        memcpy(ed->clipboard, buf + s, ed->clipboard_len);
        ed->clipboard[ed->clipboard_len] = '\0';
        printf(COLOR_RESULT "Línea %d copiada al portapapeles (%zu bytes).\n" COLOR_RESET,
               n, ed->clipboard_len);
    }
    free(buf);
}

/* --- Equipo 3: x [n]  -> pegar el portapapeles como nueva línea en la posición n --- */
static void ed_cmd_paste(EditorState *ed, const char *arg) {
    if (ed->clipboard == NULL || ed->clipboard_len == 0) {
        printf(COLOR_ERROR "El portapapeles está vacío. Usa 'y <n>' primero.\n" COLOR_RESET);
        return;
    }
    size_t len = 0;
    char *buf = ed_read_all(ed, &len);
    if (!buf) return;

    size_t s, e; int total = 0;
    size_t pos;
    if (arg == NULL) {
        pos = len; /* Sin número: pegar al final */
    } else {
        int n = atoi(arg);
        ed_line_bounds(buf, len, n, &s, &e, &total);
        if (n <= 1) pos = 0;
        else if (n > total) pos = len;
        else pos = s;
    }

    /* Asegurar que el contenido pegado termine en '\n' */
    int need_nl = (ed->clipboard[ed->clipboard_len - 1] != '\n');
    size_t ins_len = ed->clipboard_len + (need_nl ? 1 : 0);

    char *out = malloc(len + ins_len + 1);
    if (!out) { free(buf); return; }

    ed_push_undo(ed);

    memcpy(out, buf, pos);
    memcpy(out + pos, ed->clipboard, ed->clipboard_len);
    if (need_nl) out[pos + ed->clipboard_len] = '\n';
    memcpy(out + pos + ins_len, buf + pos, len - pos);

    if (ed_write_all(ed, out, len + ins_len) == 0)
        printf(COLOR_RESULT "Portapapeles pegado.\n" COLOR_RESET);

    free(out);
    free(buf);
}

/* --- Equipo 4: u  -> deshacer (undo) restaurando el último snapshot --- */
static void ed_cmd_undo(EditorState *ed) {
    if (ed->undo_top <= 0) {
        printf(COLOR_INFO "Nada que deshacer.\n" COLOR_RESET);
        return;
    }

    /* Guardar el estado actual en la pila de redo antes de revertir */
    size_t cur_len = 0;
    char *cur = ed_read_all(ed, &cur_len);
    if (cur) {
        if (ed_make_swap(ed, cur, cur_len, ed->redo_stack[ed->redo_top]) == 0)
            ed->redo_top++;
        free(cur);
    }

    /* Restaurar el snapshot anterior */
    ed->undo_top--;
    size_t slen = 0;
    char *snap = ed_read_swap(ed->undo_stack[ed->undo_top], &slen);
    if (snap) {
        ed_write_all(ed, snap, slen);
        free(snap);
        LOG_SYSCALL("unlink", "\"%s\"", ed->undo_stack[ed->undo_top]);
        unlink(ed->undo_stack[ed->undo_top]);
        LOG_SYSCALL_RESULT(0);
        printf(COLOR_RESULT "Undo aplicado.\n" COLOR_RESET);
    }
}

/* --- Equipo 4: r  -> rehacer (redo) restaurando el último estado deshecho --- */
static void ed_cmd_redo(EditorState *ed) {
    if (ed->redo_top <= 0) {
        printf(COLOR_INFO "Nada que rehacer.\n" COLOR_RESET);
        return;
    }

    /* Guardar el estado actual en la pila de undo antes de rehacer */
    size_t cur_len = 0;
    char *cur = ed_read_all(ed, &cur_len);
    if (cur) {
        if (ed_make_swap(ed, cur, cur_len, ed->undo_stack[ed->undo_top]) == 0)
            ed->undo_top++;
        free(cur);
    }

    ed->redo_top--;
    size_t slen = 0;
    char *snap = ed_read_swap(ed->redo_stack[ed->redo_top], &slen);
    if (snap) {
        ed_write_all(ed, snap, slen);
        free(snap);
        LOG_SYSCALL("unlink", "\"%s\"", ed->redo_stack[ed->redo_top]);
        unlink(ed->redo_stack[ed->redo_top]);
        LOG_SYSCALL_RESULT(0);
        printf(COLOR_RESULT "Redo aplicado.\n" COLOR_RESET);
    }
}

/* Imprime la ayuda de los subcomandos disponibles dentro del editor */
static void ed_help(void) {
    printf(COLOR_TITLE "\n--- Editor de Texto CLI (subcomandos) ---\n" COLOR_RESET);
    printf("  " COLOR_PROMPT "o <archivo>" COLOR_RESET "   Abre/crea otro archivo en la sesión.\n");
    printf("  " COLOR_PROMPT "p [n]" COLOR_RESET "         Imprime la línea n (o todo el archivo).\n");
    printf("  " COLOR_PROMPT "a <texto>" COLOR_RESET "     Añade texto como nueva línea al final.\n");
    printf("  " COLOR_PROMPT "d <n>" COLOR_RESET "         Borra la línea n.\n");
    printf("  " COLOR_PROMPT "i <n> <texto>" COLOR_RESET " Inserta texto en la línea n (desplaza el resto).\n");
    printf("  " COLOR_PROMPT "s <palabra>" COLOR_RESET "   Busca una palabra y lista las líneas.\n");
    printf("  " COLOR_PROMPT "m" COLOR_RESET "             Metadatos: tamaño, permisos, inodo, modificación.\n");
    printf("  " COLOR_PROMPT "y <n>" COLOR_RESET "         Copia (yank) la línea n al portapapeles.\n");
    printf("  " COLOR_PROMPT "x [n]" COLOR_RESET "         Pega el portapapeles en la línea n (o al final).\n");
    printf("  " COLOR_PROMPT "u" COLOR_RESET "             Deshacer (undo) el último cambio.\n");
    printf("  " COLOR_PROMPT "r" COLOR_RESET "             Rehacer (redo) el último cambio deshecho.\n");
    printf("  " COLOR_PROMPT "h" COLOR_RESET "             Muestra esta ayuda.\n");
    printf("  " COLOR_PROMPT "q" COLOR_RESET "             Cierra el archivo y vuelve al shell.\n\n");
}

/* Abre (o crea) un archivo con open(O_RDWR|O_CREAT). Cierra el FD previo si existía. */
static int ed_open_file(EditorState *ed, const char *filename) {
    LOG_SYSCALL("open", "\"%s\", O_RDWR|O_CREAT, 0644", filename);
    int fd = open(filename, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return -1;
    }
    LOG_SYSCALL_RESULT(fd);

    if (ed->fd >= 0) {
        LOG_SYSCALL("close", "%d", ed->fd);
        close(ed->fd);
        LOG_SYSCALL_RESULT(0);
    }
    ed->fd = fd;
    strncpy(ed->filename, filename, sizeof(ed->filename) - 1);
    ed->filename[sizeof(ed->filename) - 1] = '\0';
    return 0;
}

/* Libera todos los recursos del editor: swaps en /tmp (unlink), portapapeles y FD. */
static void ed_cleanup(EditorState *ed) {
    for (int i = 0; i < ed->undo_top; i++) {
        LOG_SYSCALL("unlink", "\"%s\"", ed->undo_stack[i]);
        unlink(ed->undo_stack[i]);
        LOG_SYSCALL_RESULT(0);
    }
    for (int i = 0; i < ed->redo_top; i++) {
        LOG_SYSCALL("unlink", "\"%s\"", ed->redo_stack[i]);
        unlink(ed->redo_stack[i]);
        LOG_SYSCALL_RESULT(0);
    }
    free(ed->clipboard);
    ed->clipboard = NULL;

    if (ed->fd >= 0) {
        LOG_SYSCALL("close", "%d", ed->fd);
        close(ed->fd);
        LOG_SYSCALL_RESULT(0);
        ed->fd = -1;
    }
}

/**
 * ====================================================================================
 * COMANDO PRINCIPAL: editor [archivo]
 * ====================================================================================
 * Punto de entrada registrado en la tabla del shell. Arranca el SUB-REPL del editor.
 *
 * Si recibe un argumento, abre/crea ese archivo de inmediato (equivalente a ejecutar
 * `o <archivo>` al entrar). Luego entra en un ciclo interactivo propio que lee comandos
 * desde STDIN con fgets y los enruta a los subcomandos. El ciclo termina con `q`.
 */
int cmd_editor(int argc, char **argv) {
    EditorState ed;
    memset(&ed, 0, sizeof(ed));
    ed.fd = -1;
    ed.undo_top = 0;
    ed.redo_top = 0;
    ed.clipboard = NULL;
    ed.clipboard_len = 0;
    ed.swap_counter = 0;

    printf(COLOR_TITLE "\n=== Editor de Texto CLI (EAFITOS) ===\n" COLOR_RESET);
    printf(COLOR_INFO "Editor interactivo basado en syscalls (open/read/write/lseek/ftruncate/close).\n" COLOR_RESET);
    printf(COLOR_INFO "Escribe 'h' para ayuda, 'q' para salir y volver al shell.\n" COLOR_RESET);

    /* Si se pasó un archivo como argumento, abrirlo de una vez */
    if (argc >= 2) {
        if (ed_open_file(&ed, argv[1]) == 0)
            printf(COLOR_RESULT "Archivo '%s' abierto.\n" COLOR_RESET, ed.filename);
    } else {
        printf(COLOR_INFO "No se indicó archivo. Usa 'o <archivo>' para abrir o crear uno.\n" COLOR_RESET);
    }

    char line[ED_MAX_LINE];
    while (1) {
        if (ed.fd >= 0)
            printf(COLOR_PROMPT "editor:%s> " COLOR_RESET, ed.filename);
        else
            printf(COLOR_PROMPT "editor> " COLOR_RESET);
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            break; /* EOF (Ctrl+D): salir del editor */
        }

        /* Quitar el salto de línea final */
        size_t ll = strlen(line);
        if (ll > 0 && line[ll - 1] == '\n') line[--ll] = '\0';

        if (ll == 0) continue; /* línea vacía */

        char cmd = line[0];
        /* Puntero al "resto" tras el comando y el espacio separador */
        char *rest = (ll >= 2) ? line + 2 : NULL;
        if (rest && *rest == '\0') rest = NULL;

        /* La mayoría de subcomandos requieren un archivo abierto */
        if (cmd != 'o' && cmd != 'q' && cmd != 'h' && ed.fd < 0) {
            printf(COLOR_ERROR "No hay archivo abierto. Usa 'o <archivo>' primero.\n" COLOR_RESET);
            continue;
        }

        switch (cmd) {
            case 'o': { /* Abrir/crear archivo */
                if (rest == NULL) { printf(COLOR_ERROR "Uso: o <archivo>\n" COLOR_RESET); break; }
                if (ed_open_file(&ed, rest) == 0)
                    printf(COLOR_RESULT "Archivo '%s' abierto.\n" COLOR_RESET, ed.filename);
                break;
            }
            case 'p': ed_cmd_print(&ed, rest); break;        /* Base */
            case 'a': ed_cmd_append(&ed, rest); break;       /* Base */
            case 'd': ed_cmd_delete(&ed, rest); break;       /* Base */
            case 'i': {                                      /* Equipo 2: i <n> <texto> */
                /* Separar el primer token (n) del resto (texto) */
                char *n_tok = rest;
                char *txt = NULL;
                if (rest) {
                    char *sp = strchr(rest, ' ');
                    if (sp) { *sp = '\0'; txt = sp + 1; }
                }
                ed_cmd_insert(&ed, n_tok, txt);
                break;
            }
            case 's': ed_cmd_search(&ed, rest); break;       /* Equipo 2 */
            case 'm': ed_cmd_meta(&ed); break;               /* Equipo 3 */
            case 'y': ed_cmd_yank(&ed, rest); break;         /* Equipo 3 */
            case 'x': ed_cmd_paste(&ed, rest); break;        /* Equipo 3 */
            case 'u': ed_cmd_undo(&ed); break;               /* Equipo 4 */
            case 'r': ed_cmd_redo(&ed); break;               /* Equipo 4 */
            case 'h': ed_help(); break;
            case 'q': /* Salir: cerrar FD, limpiar swaps y portapapeles */
                ed_cleanup(&ed);
                printf(COLOR_INFO "Saliendo del editor. Regresando al shell.\n" COLOR_RESET);
                return 0;
            default:
                printf(COLOR_ERROR "Subcomando '%c' no reconocido. Escribe 'h' para ayuda.\n" COLOR_RESET, cmd);
                break;
        }
    }

    /* Salida por EOF: asegurar limpieza de recursos */
    ed_cleanup(&ed);
    return 0;
}
