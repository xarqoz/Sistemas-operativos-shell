#!/usr/bin/env bash
# ====================================================================================
# Script de pruebas del Editor de Texto CLI (categoría "editor") - SO2026B
# ====================================================================================
# Valida de forma automatizada los comandos del editor integrado al shell educativo,
# cubriendo los requisitos ACUMULATIVOS para un equipo de 4 integrantes:
#   - Base:      o, p, a, d, q
#   - Equipo 2:  i (inserción), s (búsqueda)
#   - Equipo 3:  m (metadatos/fstat), y (copiar), x (pegar)
#   - Equipo 4:  u (undo), r (redo)  [archivos swap en /tmp + unlink]
#
# Uso:  ./test_editor.sh
# Requiere: el binario eafitOS compilado (make all) en el directorio actual.
# ====================================================================================
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$DIR/eafitOS"
TESTFILE="/tmp/editor_test_$$.txt"
OUT="/tmp/editor_test_out_$$.txt"

# Elimina los códigos de color ANSI para facilitar las comprobaciones de texto
strip() { sed 's/\x1b\[[0-9;]*m//g'; }

fail=0
check() {
    # check "<descripción>" "<patrón esperado>"
    if grep -qF "$2" "$OUT"; then
        echo "  [OK]   $1"
    else
        echo "  [FALLA] $1 (no se encontró: '$2')"
        fail=1
    fi
}

# Compilar si el binario no existe
if [ ! -x "$BIN" ]; then
    echo ">> Binario no encontrado; compilando con make..."
    ( cd "$DIR" && echo exit | make all >/dev/null 2>&1 )
fi

rm -f "$TESTFILE"

echo "=== Ejecutando sesión de prueba del editor ==="
printf '%s\n' \
    "editor $TESTFILE" \
    'a Primera linea' \
    'a Segunda linea' \
    'a Tercera linea' \
    'p' \
    'p 2' \
    'i 2 Linea insertada' \
    's linea' \
    'y 1' \
    'x 4' \
    'm' \
    'd 1' \
    'u' \
    'r' \
    'q' \
    'exit' | "$BIN" 2>&1 | strip > "$OUT"

echo "=== Verificaciones ==="
check "Base 'o': abre archivo con open(O_RDWR|O_CREAT)" "O_RDWR|O_CREAT"
check "Base 'a': añade línea al final"                   "Línea añadida al final."
check "Base 'p': imprime todo el contenido"             "Primera linea"
check "Base 'p 2': imprime línea específica"            "Segunda linea"
check "Equipo2 'i': inserción arbitraria"               "Texto insertado en la línea 2."
check "Equipo2 's': búsqueda simple"                    "coincidencia(s) encontrada(s)."
check "Equipo3 'm': metadatos con fstat"                "Metadatos del Archivo (fstat)"
check "Equipo3 'y': copiar al portapapeles"             "copiada al portapapeles"
check "Equipo3 'x': pegar portapapeles"                 "Portapapeles pegado."
check "Base 'd': borra línea (ftruncate)"               "borrada."
check "Equipo4 'u': deshacer"                           "Undo aplicado."
check "Equipo4 'r': rehacer"                            "Redo aplicado."
check "Base 'q'/limpieza: unlink de swaps + salida"     "Regresando al shell."

echo "=== Verificación de recursos ==="
if ls /tmp/eafitos_edit_* >/dev/null 2>&1; then
    echo "  [FALLA] Quedaron archivos swap huérfanos en /tmp"
    fail=1
else
    echo "  [OK]   No quedaron archivos swap huérfanos en /tmp"
fi

rm -f "$TESTFILE" "$OUT"

echo
if [ "$fail" -eq 0 ]; then
    echo "RESULTADO: TODAS LAS PRUEBAS PASARON ✔"
    exit 0
else
    echo "RESULTADO: HAY PRUEBAS FALLIDAS"
    exit 1
fi
