CC = gcc
CFLAGS = -std=gnu99 -O0 -g -shared -fPIC -Wall -I /usr/local/include
portaudio_sources = portaudio.c
installdir = /usr/local/lib/deadbeef

ifeq ($(OS),Windows_NT)
    suffix = dll
else
    suffix = so
endif

all:
	$(CC) $(CFLAGS) -o portaudio.$(suffix) $(portaudio_sources) -lportaudio

install:
	cp portaudio.so $(installdir)
