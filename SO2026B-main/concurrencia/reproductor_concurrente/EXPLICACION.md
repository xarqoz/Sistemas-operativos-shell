# Reproductor de Audio Concurrente — Parcial 2, Alternativa 2

**Curso:** Sistemas Operativos (SO2026B)
**Tema:** Concurrencia, hilos (pthreads), exclusión mutua, sincronización y condiciones de carrera.

Reproductor de audio por línea de comandos cuyo **foco es el diseño concurrente**: el
motor de reproducción (productor-consumidor) está desacoplado de la gestión dinámica y
concurrente de la lista de reproducción (lectores-escritores). Toda la sincronización se
implementa **a mano** con primitivas POSIX (`pthread`, `mutex`, variables de condición y
`rwlock`), sin librerías de alto nivel que la resuelvan automáticamente.

---

## 1. Cumplimiento de las Instrucciones Generales del parcial

| Instrucción | Cómo se cumple |
|-------------|----------------|
| C bajo POSIX/Linux | Código C (C11/GNU), syscalls POSIX (`open`, `read`, `write`, `pipe`, `fork`, `execlp`, `fstat`, `waitpid`). |
| Uso explícito de primitivas de concurrencia | `pthread_create/join`, `pthread_mutex_t`, `pthread_cond_t`, `pthread_rwlock_t` usados directamente. |
| Prohibido librerías de alto nivel de sincronización | No se usan colas/pools "mágicos"; el búfer circular y la playlist se sincronizan manualmente. |

---

## 2. Arquitectura de hilos

```
            ┌───────────────────────────────────────────────┐
            │   HILO PRINCIPAL (CLI / UI)  — main.c          │
            │   Lee comandos y los traduce a control async   │
            └───────┬───────────────────────────┬───────────┘
      control async │ (mutex + condvar)          │ lectura/escritura (rwlock)
                    ▼                             ▼
      ┌──────────────────────────┐   ┌───────────────────────────────┐
      │ HILO GESTOR (manager)    │   │  PLAYLIST compartida           │
      │ recorre la cola y lanza  │──▶│  (lista doblemente enlazada +  │
      │ productor+consumidor     │   │   pthread_rwlock)              │
      └───────┬──────────────────┘   └───────────────────────────────┘
              │ crea por pista
     ┌────────▼─────────┐   RingBuffer (mutex + 2 condvars)   ┌───────────────────┐
     │ HILO PRODUCTOR   │ ─────────────────────────────────▶ │ HILO CONSUMIDOR    │
     │ open()+read()    │     búfer circular acotado          │ rb_read()+write()  │
     │ → rb_write()     │                                     │ → ffplay (pipe)    │
     └──────────────────┘                                     └───────────────────┘
```

### Módulos (diseño modular — 15 pts)
| Archivo | Responsabilidad |
|---------|-----------------|
| `main.c` | Interfaz CLI; traduce comandos del usuario a control asíncrono y a operaciones de playlist. |
| `audio_engine.c/.h` | Motor: hilo gestor + hilos productor/consumidor por pista + estado de control. |
| `ring_buffer.c/.h` | Búfer circular acotado sincronizado (productor-consumidor). |
| `playlist.c/.h` | Cola de reproducción concurrente (lectores-escritores con `rwlock`). |

---

## 3. Búfer circular productor-consumidor (25 pts)

Archivo `ring_buffer.c`. Es un **bounded buffer** protegido por **un mutex** y **dos
variables de condición**:

- `not_full`: el **productor** duerme aquí cuando el búfer está lleno (previene
  **sobrellenado**).
- `not_empty`: el **consumidor** duerme aquí cuando el búfer está vacío y el productor
  sigue activo (previene **subdesbordamiento / underrun**).

**Sin espera activa (busy-waiting):** ambos usan `pthread_cond_wait`, que libera el mutex
y bloquea el hilo hasta recibir un `signal`. Fragmento del productor:

```c
while (rb->size == rb->capacity && !rb->shutdown)
    pthread_cond_wait(&rb->not_full, &rb->mutex);   // duerme, no quema CPU
...
pthread_cond_signal(&rb->not_empty);                // avisa que hay datos
```

Y del consumidor:

```c
while (rb->size == 0 && !rb->producer_done && !rb->shutdown)
    pthread_cond_wait(&rb->not_empty, &rb->mutex);  // duerme hasta que haya datos
...
pthread_cond_signal(&rb->not_full);                 // avisa que hay espacio
```

El uso de `while` (no `if`) alrededor del `wait` protege contra *spurious wakeups* y
reevalúa siempre la condición al despertar.

---

## 4. Gestión concurrente de la playlist (25 pts)

Archivo `playlist.c`. Lista **doblemente enlazada** protegida por un
**`pthread_rwlock_t`** (patrón **lectores-escritores**):

- **Lecturas simultáneas** (`pthread_rwlock_rdlock`): `playlist_get_current`,
  `playlist_count`, `playlist_print`. El hilo gestor es un lector frecuente.
- **Escrituras exclusivas** (`pthread_rwlock_wrlock`): `add`, `remove`, `move`, `clear`,
  `select`, `next`, `prev`.

**Protección contra uso-después-de-liberar:** `playlist_get_current` **copia** la ruta y
el nombre de la pista actual a búferes del llamador en lugar de devolver el puntero al
nodo. Así, si el usuario elimina la pista que está sonando, el motor sigue trabajando con
su copia local y nunca accede a memoria liberada (evita el *segmentation fault* que
advierte el enunciado). El cursor `current` se reubica automáticamente al eliminar su nodo.

---

## 5. Control asíncrono de eventos (20 pts)

Archivo `audio_engine.c`. El estado (`ST_PLAYING`/`ST_PAUSED`/`ST_STOPPED`) y las
peticiones (`request_next/prev/stop`, `quit`) están protegidos por `ctl_mutex` y se
señalizan con `ctl_cond`. El hilo de la CLI nunca se bloquea: solo marca el estado y hace
`pthread_cond_broadcast`.

**PAUSE sin espera activa:** el consumidor, en cada iteración, llama a `wait_if_paused`,
que duerme en la condvar mientras el estado sea `ST_PAUSED`:

```c
while (e->state == ST_PAUSED &&
       !e->request_stop && !e->request_next && !e->request_prev && !e->quit)
    pthread_cond_wait(&e->ctl_cond, &e->ctl_mutex);  // pausa real: 0% CPU
```

`next/prev/stop` llaman además a `rb_shutdown()` para desbloquear al instante a los hilos
de la pista actual y responder sin latencia apreciable.

---

## 6. Robustez y ausencia de deadlocks (15 pts)

- **Orden de locks:** nunca se anidan `ctl_mutex` y el mutex del `RingBuffer` tomándolos
  en órdenes opuestos, por lo que no hay posibilidad de interbloqueo circular.
- **Apagado limpio:** `engine_shutdown` fija `quit`, hace `broadcast` y `rb_shutdown`
  (despierta toda espera), y luego `pthread_join` del hilo gestor; cada pista hace `join`
  de su productor y consumidor.
- **Recursos:** cada `open` tiene su `close`; cada pipe se cierra y su `ffplay` se recoge
  con `waitpid` (sin zombies); `rb_destroy` y `playlist_destroy` liberan toda la memoria
  dinámica y destruyen mutex/condvars/rwlock.
- **Señales:** `SIGINT` (Ctrl+C) se captura para salir ordenadamente en vez de abortar.

---

## 7. Salida de audio y portabilidad

El consumidor envía el MP3 a **`ffplay`** mediante `fork()` + `pipe()` + `dup2()` +
`execlp()` (mismo patrón IPC de los ejemplos del curso). Si `ffplay` no está instalado
(p. ej. en un contenedor sin audio), el motor usa un **sumidero temporizado** que simula
el consumo a la tasa de reproducción con `nanosleep`, de modo que **la concurrencia se
puede validar aunque no haya hardware de sonido**.

> Para audio real: `sudo apt install ffmpeg` (provee `ffplay`).

---

## 8. Compilación, ejecución y pruebas

```bash
make all            # compila (enlaza con -pthread), sin warnings
make run            # ejecuta precargando 3 canciones de ejemplo
./test_concurrencia.sh   # batería de pruebas de concurrencia
```

### Comandos de la CLI
`add <ruta>`, `rm <i>`, `move <i> <j>`, `list`, `clear`, `sel <i>`, `play`, `pause`,
`pp` (toggle), `stop`, `next`, `prev`, `status`, `help`, `quit`.

### Verificación de condiciones de carrera (en una máquina con los sanitizers)
```bash
gcc -fsanitize=thread  -pthread -o rc_tsan  *.c -pthread   # detecta data races
gcc -fsanitize=address -pthread -o rc_asan  *.c -pthread   # detecta leaks
```
