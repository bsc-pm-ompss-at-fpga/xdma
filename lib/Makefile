CC = ${CROSS_COMPILE}gcc
LDFLAGS := -lxdma
CFLAGS := -c -Wall -Werror -std=c99 -g
INCLUDES := -I. -I../dev


.PHONY : all
all : libxdma


libxdma.o : libxdma.c libxdma.h
	$(CC) $(CFLAGS) $(INCLUDES) -fpic libxdma.c


libxdma : libxdma.o
	$(CC) -shared -Wl,-soname,libxdma.so -o libxdma.so libxdma.o


.PHONY : clean
clean :
	rm -f *.o libxdma.so
