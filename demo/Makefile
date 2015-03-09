CC = arm-xilinx-linux-gnueabi-gcc
LDFLAGS := -L../lib -lxdma
CFLAGS := -c -Wall -g -O0
INCLUDES := -I. -I../dev -I../lib

EXECUTABLE = \
	app \
	demo \
	test \
	len-test \
	gen-task


.PHONY : all
all: $(EXECUTABLE)


test : xdma-test.o
	$(CC) $< -o $@ $(LDFLAGS)


demo : xdma-demo.o
	$(CC) $< -o $@ $(LDFLAGS)


app : xdma-app.o
	$(CC) $< -o $@ $(LDFLAGS)

len-test : xdma-len-test.o
	$(CC) $< -o $@ $(LDFLAGS)

gen-task : xdma-gen-task.o
	$(CC) $< -o $@ $(LDFLAGS)


.c.o:
	$(CC) $(INCLUDES) $(CFLAGS) $< -o $@


.PHONY : clean
clean:
	rm -f *.o $(EXECUTABLE)
