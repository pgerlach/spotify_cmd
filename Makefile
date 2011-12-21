SRC = src/main.c src/credentials.c src/spotify_appkey.c src/audio.c

CC=gcc
TARGET=bin/spotify_cmd


ifeq ($(shell uname),Darwin)
SRC+=src/osx-audio.c
CFLAGS=`pkg-config --cflags libevent`
LDFLAGS=-framework libspotify -levent -levent_pthreads -framework AudioToolbox
else
ifeq ($(shell uname -p),x86_64)
SRC+=src/alsa-audio.c
CFLAGS=`pkg-config --cflags libevent libevent_pthreads libspotify alsa`
LDFLAGS=`pkg-config --libs libevent libevent_pthreads libspotify alsa`
else
CFLAGS=-I/home/pierre/work/perso/noisebox/chumby-buildroot/usr/local/include
LDFLAGS=-L/home/pierre/work/perso/noisebox/chumby-buildroot/usr/local/lib -lspotify -levent -levent_pthreads
endif
endif

OBJS = ${SRC:.c=.o}


all: ${OBJS}
	mkdir -p `dirname ${TARGET}`
	${CC} ${OBJS} ${LDFLAGS} -o ${TARGET}

clean:
	rm -f ${OBJS}

distclean: clean
	rm -f ${TARGET}
