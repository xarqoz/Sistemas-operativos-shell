#!/usr/bin/env bash
# ====================================================================================
# run.sh — Compila y ejecuta el Reproductor de Audio Concurrente (Parcial 2, Alt. 2)
# ====================================================================================
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
MUSIC="$DIR/../concurrencia_ludica/music"

echo "================================================="
echo "   REPRODUCTOR DE AUDIO CONCURRENTE (SO2026B)"
echo "================================================="

echo "[1] Compilando con make..."
( cd "$DIR" && make all ) || { echo "Error de compilación."; exit 1; }

echo "[2] Verificando reproductor de audio (ffplay)..."
if command -v ffplay &>/dev/null; then
    echo "    ffplay encontrado: habrá audio real."
else
    echo "    ffplay NO encontrado: se usará salida simulada (sin sonido)."
    echo "    Para audio real: sudo apt install ffmpeg"
fi

echo "[3] Lanzando el reproductor con canciones de ejemplo precargadas..."
echo "    Escribe 'help' para ver los comandos, 'quit' para salir."
echo "-------------------------------------------------"
# Precarga algunas canciones de la carpeta de ejemplo del curso (si existen).
ARGS=()
for f in canon.mp3 polka.mp3 tech.mp3; do
    [ -f "$MUSIC/$f" ] && ARGS+=("$MUSIC/$f")
done
"$DIR/reproductor_concurrente" "${ARGS[@]}"
echo "-------------------------------------------------"
echo "Finalizado."
