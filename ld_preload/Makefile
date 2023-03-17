#!/usr/bin/make -f

CFLAGS = -Wall -Wextra -std=c++17 -pedantic -Wall -O2 -fpic -shared -I../rpclib/include
LDFLAGS = -L../rpclib/build -lrpc -ldl

.PHONY: clean

preload_main.o: preload_main.c
	${CXX} ${CFLAGS} $< -o $@ ${LDFLAGS}

clean:
	${RM} *.o

