CC = gcc
CFLAGS = -std=gnu99 -O0 -g -shared -fPIC
portaudio_sources = portaudio.c

ifeq ($(OS),Windows_NT)
    suffix = dll
else
    suffix = so
endif

all:
	$(CC) $(CFLAGS) -lportaudio -o portaudio.$(suffix) $(portaudio_sources)

