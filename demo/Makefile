CC=${CROSS_COMPILE}gcc
LDFLAGS=-L../lib -lxdma
CFLAGS=-std=gnu99 -Wall -g -O0
INCLUDES=-I. -I../lib

EXECUTABLE = \
	loopback \
	gen_task \
	matmul


.PHONY : all
all: $(EXECUTABLE)

loopback: loopback.c
	$(CC) $(INCLUDES) $(CFLAGS) $< -o $@ $(LDFLAGS)

gen_task: gen_task.c
	$(CC) $(INCLUDES) $(CFLAGS) $< -o $@ $(LDFLAGS)

matmul: matmul.c
	$(CC) $(INCLUDES) $(CFLAGS) -Wno-format-security $< -o $@ $(LDFLAGS)


.PHONY : clean
clean:
	rm -f $(EXECUTABLE)
