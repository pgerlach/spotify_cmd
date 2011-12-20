SRC = src/main.c src/credentials.c src/spotify_appkey.c
OBJS = ${SRC:.c=.o}

CC=gcc
#CFLAGS=`pkg-config --cflags libevent`
LDFLAGS=-framework libspotify -levent -levent_pthreads

TARGET=bin/spotify_cmd

all: ${OBJS}
	mkdir -p `dirname ${TARGET}`
	${CC} ${OBJS} ${LDFLAGS} -o ${TARGET}

clean:
	rm -f ${OBJS}

distclean: clean
	rm -f ${TARGET}
