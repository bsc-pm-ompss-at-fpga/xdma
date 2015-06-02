CC = ${CROSS_COMPILE}gcc
LDFLAGS := -L../lib -lxdma
CFLAGS := -std=gnu99 -Wall -g -O0
INCLUDES := -I. -I../lib

EXECUTABLE = \
	loopback \
	gen_task \
	matmul


.PHONY : all
all: $(EXECUTABLE)


loopback: loopback.o
	$(CC) $< -o $@ $(LDFLAGS)

gen_task: gen_task.o
	$(CC) $< -o $@ $(LDFLAGS)

matmul: matmul.o
	$(CC) $< -o $@ $(LDFLAGS)

.c.o:
	$(CC) $(INCLUDES) $(CFLAGS) $< -o $@


.PHONY : clean
clean:
	rm -f *.o $(EXECUTABLE)
