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

.PHONY : clean
clean :
	rm -f *.o libxdma.so libxdma-static.a
	rm -rf html latex
