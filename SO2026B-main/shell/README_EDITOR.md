# Editor de Texto CLI — Categoría `editor` (SO2026B)

Editor de texto interactivo operado estrictamente por CLI (inspirado en `ed`/`vi`),
integrado al **Shell Educativo de Syscalls**. Implementa, de forma **acumulativa**, los
requisitos de los equipos de 1, 2, 3 y 4 integrantes descritos en el enunciado del parcial.

## 1. Cómo se invoca

Desde el shell (`eafitOS`):

```
eafitOS> editor [archivo]
```

- Si se pasa `archivo`, se abre/crea de inmediato (equivale a ejecutar `o <archivo>` al entrar).
- Sin argumento, se entra al editor y luego se abre un archivo con `o <archivo>`.

El comando `editor` arranca un **sub-REPL** propio. Mientras dura la sesión, el prompt
cambia a `editor:<archivo>>`. Se vuelve al shell con `q`.

Ayuda integrada:

```
eafitOS> help              # lista las categorías, incluida 'editor'
eafitOS> help editor       # muestra el comando de la categoría
editor> h                  # ayuda de los subcomandos del editor
```

## 2. Decisión arquitectónica (clasificación de la categoría)

El enunciado pide **justificar** si el editor debe entrar en una categoría existente o en
una nueva. **Decisión: se crea una categoría nueva, `editor`.**

Razonamiento (pensamiento crítico):

- El resto de comandos del shell (`datos`, `memoria`, `monitoreo`, `utilidades`) son
  **atómicos**: se ejecutan una vez, demuestran una o pocas syscalls y retornan de
  inmediato al prompt principal.
- El editor es una **aplicación interactiva con estado persistente**: mantiene un File
  Descriptor abierto, un portapapeles, pilas de undo/redo y archivos swap temporales
  durante toda la sesión.
- Meterlo en `datos` rompería la semántica atómica de esa categoría y mezclaría
  conceptos. Por eso el comando `editor` actúa como **punto de entrada** que cede el
  control a un **ciclo REPL anidado** y lo devuelve al shell al salir, respetando la
  arquitectura de enrutamiento por tabla de comandos (`Command commands[]`) sin
  contaminar las categorías ya existentes.

## 3. Restricción crítica de I/O (cumplida)

Todo acceso al archivo de texto en disco se hace **exclusivamente** con llamadas al
sistema de bajo nivel. **No** se usa `fopen`/`fread`/`fwrite`/`fclose`.

| Operación                        | Syscalls usadas                                  |
|----------------------------------|--------------------------------------------------|
| Abrir/crear archivo              | `open(O_RDWR \| O_CREAT, 0644)`                  |
| Leer contenido / recorrer bytes  | `read`, `lseek`                                  |
| Escribir / añadir                | `write`, `lseek(SEEK_END)`                       |
| Borrar línea / reescribir        | `write`, `lseek`, `ftruncate`                    |
| Metadatos                        | `fstat`                                          |
| Swap temporal (undo/redo)        | `open`, `write`, `read`, `close`, `unlink`       |

La I/O estándar (`printf`, `fgets`) se usa **solo** para leer comandos desde STDIN e
imprimir en STDOUT, como permite el enunciado.

## 4. Subcomandos (acumulativos, equipo de 4)

| Cmd              | Nivel     | Descripción                                                        |
|------------------|-----------|--------------------------------------------------------------------|
| `o <archivo>`    | Base      | Abre/crea un archivo (`open` con `O_RDWR\|O_CREAT`).               |
| `p [n]`          | Base      | Imprime la línea `n`; sin `n`, imprime todo recorriendo bytes.     |
| `a <texto>`      | Base      | Añade `texto` como nueva línea al final (`lseek SEEK_END`+`write`).|
| `d <n>`          | Base      | Borra la línea `n` desplazando bytes y truncando (`ftruncate`).    |
| `q`              | Base      | Cierra el FD, limpia swaps y sale sin fugas de memoria.            |
| `i <n> <texto>`  | Equipo 2  | Inserción arbitraria en la línea `n` desplazando el resto.         |
| `s <palabra>`    | Equipo 2  | Búsqueda simple; lista las líneas que contienen la palabra.        |
| `m`              | Equipo 3  | Metadatos: tamaño, permisos, inodo y modificación (`fstat`).       |
| `y <n>`          | Equipo 3  | Copia (yank) la línea `n` a un portapapeles secuencial local.      |
| `x [n]`          | Equipo 3  | Pega el portapapeles en la línea `n` (o al final si no se indica). |
| `u`              | Equipo 4  | Deshacer (undo) usando snapshots swap en `/tmp`.                   |
| `r`              | Equipo 4  | Rehacer (redo) el último cambio deshecho.                          |
| `h`              | —         | Ayuda de subcomandos.                                              |

### Estrategia de Undo/Redo (Equipo 4)

Antes de cada operación destructiva se toma un *snapshot* del contenido y se guarda en un
**archivo swap** en `/tmp` (`/tmp/eafitos_edit_<pid>_<n>.swap`) con `open`/`write`/`close`.
La ruta se apila en la pila de undo. Al hacer `u` se restaura ese snapshot y la ruta pasa
a la pila de redo. **Al salir (`q`) todos los swaps se eliminan con `unlink()`** para no
dejar basura en el sistema de ficheros.

## 5. Manejo de errores

Todas las syscalls verifican su retorno (p. ej. `open` devolviendo `-1`) y usan
`strerror(errno)` a través de la macro educativa `LOG_SYSCALL_ERROR`, que imita el formato
de `strace` resaltando la llamada, sus argumentos y el resultado.

## 6. Gestión de memoria

Los búferes dinámicos (`malloc`/`realloc`) para leer el archivo, el portapapeles y los
snapshots se liberan siempre con `free`. Al salir, `ed_cleanup()` libera el portapapeles,
cierra el FD y hace `unlink` de los swaps restantes. No quedan fugas ni archivos huérfanos.

## 7. Compilación y pruebas

```bash
make all          # compila y ejecuta el shell (target 'all')
make clean        # elimina binario y objetos
./test_editor.sh  # script de pruebas automatizado del editor
```

El script `test_editor.sh` ejecuta una sesión completa alimentando STDIN y verifica cada
subcomando (base + equipos 2/3/4), además de confirmar que no quedan archivos swap
huérfanos en `/tmp`.
