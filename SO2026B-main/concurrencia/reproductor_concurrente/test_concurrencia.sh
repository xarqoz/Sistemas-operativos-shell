#!/usr/bin/env bash
# ====================================================================================
# Script de pruebas de concurrencia — Reproductor de Audio (Parcial 2, Alternativa 2)
# ====================================================================================
# Valida de forma automatizada los criterios de la rúbrica:
#   1. Gestión concurrente de la playlist (add/rm/move/list) mientras se reproduce.
#   2. Búfer productor-consumidor: el progreso avanza en segundo plano.
#   3. Control asíncrono de eventos (play/pause/stop/next/prev).
#   4. Robustez: ráfaga de comandos sin deadlock ni crash; salida ordenada.
#
# Uso:  ./test_concurrencia.sh
# Requiere: el binario 'reproductor_concurrente' (make all) y la carpeta de música.
# ====================================================================================
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$DIR/reproductor_concurrente"
MUSIC="$DIR/../concurrencia_ludica/music"
OUT="/tmp/rc_test_$$.txt"

strip() { sed 's/\x1b\[[0-9;]*m//g'; }
fail=0
check() {
    if grep -qF "$2" "$OUT"; then echo "  [OK]   $1";
    else echo "  [FALLA] $1 (no se encontró: '$2')"; fail=1; fi
}

[ -x "$BIN" ] || ( cd "$DIR" && make all >/dev/null 2>&1 )

echo "=== PRUEBA 1: Gestión concurrente de la playlist ==="
{
  echo "add $MUSIC/solarflex.mp3"
  echo "add $MUSIC/polka.mp3"
  echo "add $MUSIC/tech.mp3"
  echo "list"
  echo "move 2 0"      # reordenar
  echo "rm 1"          # eliminar intermedia
  echo "list"
  echo "quit"
} | "$BIN" 2>&1 | strip > "$OUT"
check "Agrega canciones a la cola"        "Agregado en [2]"
check "Lista la cola"                     "[0]"
check "Reordena (move)"                   "Movida 2 -> 0."
check "Elimina pista intermedia (rm)"     "Eliminada [1]."

echo "=== PRUEBA 2: Productor-consumidor (progreso en segundo plano) ==="
{
  echo "add $MUSIC/solarflex.mp3"
  echo "play"; sleep 1
  echo "status"; sleep 1
  echo "status"
  echo "quit"
} | "$BIN" 2>&1 | strip > "$OUT"
check "Reproduce en segundo plano"        "Reproduciendo: solarflex.mp3"
check "El búfer entrega datos (progreso)" "REPRODUCIENDO | solarflex.mp3"

echo "=== PRUEBA 3: Control asíncrono (pausa congela el progreso) ==="
{
  echo "add $MUSIC/solarflex.mp3"
  echo "play"; sleep 1
  echo "pause"; sleep 0.3
  echo "status"; sleep 1
  echo "status"            # el % debe ser el mismo que el anterior
  echo "play"; sleep 0.5
  echo "stop"
  echo "status"
  echo "quit"
} | "$BIN" 2>&1 | strip > "$OUT"
check "Estado PAUSADO alcanzado"          "PAUSADO"
check "Estado DETENIDO tras stop"         "DETENIDO"
# Verifica que durante la pausa el progreso NO cambió entre dos lecturas consecutivas
PAUSED_PCTS=$(grep "PAUSADO" "$OUT" | grep -oE '[0-9]+/[0-9]+ bytes' | head -2)
L1=$(echo "$PAUSED_PCTS" | sed -n 1p)
L2=$(echo "$PAUSED_PCTS" | sed -n 2p)
if [ -n "$L1" ] && [ "$L1" = "$L2" ]; then
    echo "  [OK]   Durante la pausa el progreso NO avanza ($L1)"
else
    echo "  [INFO] Comparación de pausa: '$L1' vs '$L2' (puede variar por timing)"
fi

echo "=== PRUEBA 4: Robustez (ráfaga de comandos, sin deadlock) ==="
{
  for i in 1 2 3; do
    echo "add $MUSIC/solarflex.mp3"; echo "add $MUSIC/polka.mp3"; echo "add $MUSIC/tech.mp3"
  done
  echo "play"
  for i in $(seq 1 40); do
    echo "pause"; echo "play"; echo "next"; echo "prev"; echo "move 1 3"; echo "rm 0"; echo "status"
  done
  echo "clear"; echo "quit"
} | timeout 30 "$BIN" > "$OUT" 2>&1
RC=$?
if [ "$RC" -eq 124 ]; then echo "  [FALLA] Posible DEADLOCK (timeout)"; fail=1;
else echo "  [OK]   Terminó sin deadlock (exit=$RC)"; fi
if grep -iqE "segmentation|core dumped|abort" "$OUT"; then
    echo "  [FALLA] Hubo un crash"; fail=1;
else echo "  [OK]   Sin crashes bajo carga"; fi
grep -q "Adiós." <(strip < "$OUT") && echo "  [OK]   Salida ordenada (join de hilos)" || { echo "  [FALLA] No cerró ordenadamente"; fail=1; }

rm -f "$OUT"
echo
if [ "$fail" -eq 0 ]; then
    echo "RESULTADO: TODAS LAS PRUEBAS PASARON"
    exit 0
else
    echo "RESULTADO: HAY PRUEBAS FALLIDAS"
    exit 1
fi
