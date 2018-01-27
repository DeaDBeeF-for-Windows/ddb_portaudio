CC = gcc
CFLAGS = -std=gnu99 -O0 -g -shared -fPIC -Wall
portaudio_sources = portaudio.c
installdir = /usr/local/lib/deadbeef

ifeq ($(OS),Windows_NT)
    suffix = dll
else
    suffix = so
endif

all:
	$(CC) $(CFLAGS) -lportaudio -o portaudio.$(suffix) $(portaudio_sources)

install:
	cp portaudio.so $installdir
