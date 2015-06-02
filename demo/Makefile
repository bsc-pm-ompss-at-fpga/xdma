CC = arm-xilinx-linux-gnueabi-gcc
LDFLAGS := -L.. -lxdma
CFLAGS := -std=c99 -c -Wall -g -O0
INCLUDES := -I. -I..

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
