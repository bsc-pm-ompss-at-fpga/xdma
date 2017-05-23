CC = ${CROSS_COMPILE}gcc
LDFLAGS := -lxdma
CFLAGS := -c -Wall -Werror -std=c99 -g
INCLUDES := -I. -I../driver

.PHONY : all
all : libxdma.so libxdma-static.a


libxdma.o : libxdma.c libxdma.h
	$(CC) $(CFLAGS) $(INCLUDES) -fpic libxdma.c


libxdma.so : libxdma.o
	$(CC) -shared -Wl,-soname,libxdma.so -o libxdma.so libxdma.o

libxdma-static.a : libxdma.o
	ar rcs $@ $^

doc:
	doxygen

install: libxdma.so libxdma-static.a libxdma.h
	mkdir -p $(PREFIX)/lib
	cp libxdma.so libxdma-static.a $(PREFIX)/lib/
	mkdir -p $(PREFIX)/include
	cp libxdma.h $(PREFIX)/include/

.PHONY : clean
clean :
	rm -f *.o libxdma.so libxdma-static.a
	rm -rf html latex
