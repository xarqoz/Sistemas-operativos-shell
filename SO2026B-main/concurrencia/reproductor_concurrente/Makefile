# ====================================================================================
# Makefile — Reproductor de Audio Concurrente (Parcial 2, Alternativa 2)
# ====================================================================================
# Compila el reproductor enlazando con la librería de hilos POSIX (-pthread).
# Targets: all (compila), run (ejecuta con la música de ejemplo), clean (limpia).
# ====================================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -std=gnu11 -g -D_GNU_SOURCE -pthread
LDFLAGS = -pthread
TARGET  = reproductor_concurrente
SRCS    = main.c audio_engine.c ring_buffer.c playlist.c
OBJS    = $(SRCS:.c=.o)

# Carpeta con los MP3 de ejemplo (provista en el repo del curso)
MUSIC_DIR = ../concurrencia_ludica/music

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

%.o: %.c playlist.h ring_buffer.h audio_engine.h
	$(CC) $(CFLAGS) -c $< -o $@

# Ejecuta el reproductor precargando algunas canciones de ejemplo en la cola.
run: all
	./$(TARGET) $(MUSIC_DIR)/canon.mp3 $(MUSIC_DIR)/polka.mp3 $(MUSIC_DIR)/tech.mp3

# Ejecuta el script de pruebas de concurrencia.
test: all
	./test_concurrencia.sh

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all run test clean
