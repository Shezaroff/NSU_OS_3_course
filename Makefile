TARGET = proxy_server
SRCS = dynamic_buffer.c http_request.c http_utils.c proxy_server.c cache_map.c cleanup_thread.c

CC=gcc
RM=rm
CFLAGS = -g -O0 -Wall -Wextra
LIBS=-lpthread
INCLUDE_DIR= -I. 

all: ${TARGET}

${TARGET}: ${SRCS}
	${CC} ${CFLAGS} ${INCLUDE_DIR} ${SRCS} ${LIBS} -o ${TARGET}

clean:
	${RM} -f *.o ${TARGET}
