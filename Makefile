SRC = src/main.c src/credentials.c src/spotify_appkey.c src/audio.c

CC=gcc
CFLAGS=-Wall -O2
TARGET=bin/spotify_cmd

include config.mk

OBJS = ${SRC:.c=.o}


all: ${OBJS}
	mkdir -p `dirname ${TARGET}`
	${CC} ${OBJS} ${LDFLAGS} -o ${TARGET}

clean:
	rm -f ${OBJS}

distclean: clean
	rm -f ${TARGET}
