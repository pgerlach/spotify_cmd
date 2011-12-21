SRC = src/main.c src/credentials.c src/spotify_appkey.c src/audio.c

CC=gcc
TARGET=bin/spotify_cmd


ifeq ($(shell uname),Darwin)
# mac os
SRC+=src/osx-audio.c
CFLAGS=`pkg-config --cflags libevent`
LDFLAGS=-framework libspotify -levent -levent_pthreads -framework AudioToolbox
else
SRC+=src/alsa-audio.c
ifeq ($(shell uname -p),x86_64)
# linux desktop
CFLAGS=`pkg-config --cflags libevent libevent_pthreads libspotify alsa`
LDFLAGS=`pkg-config --libs libevent libevent_pthreads libspotify alsa`
else 
ifeq ($(shell uname -h),beagleboard)
# beagleboard
CFLAGS=`PKG_CONFIG_PATH=/usr/local/lib/ pkg-config --cflags libevent libevent_pthreads libspotify alsa`
LDFLAGS=`PKG_CONFIG_PATH=/usr/local/lib/ pkg-config --libs libevent libevent_pthreads libspotify alsa`
else
# chumby
CFLAGS=-I/home/pierre/work/perso/noisebox/chumby-buildroot/usr/local/include
LDFLAGS=-L/home/pierre/work/perso/noisebox/chumby-buildroot/usr/local/lib -lspotify -levent -levent_pthreads
endif
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
