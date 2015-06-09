CC=${CROSS_COMPILE}gcc
LDFLAGS=-L../lib -lxdma
CFLAGS=-std=gnu99 -Wall -g -O0
INCLUDES=-I. -I../lib

FPGACC=$(CROSS_COMPILE)fpgacc
FPGACFLAGS=--ompss -std=c99 -k

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


loopback_ompss: loopback_ompss.c
	$(FPGACC) $(FPGACFLAGS) $< -o $@

gen-task_ompss: gen-task_ompss.c
	$(FPGACC) $(FPGACFLAGS) $< -o $@

matmul_ompss: matmul_ompss.c
	$(FPGACC) $(FPGACFLAGS) $< -o $@


.PHONY : clean
clean:
	rm -f $(EXECUTABLE) hls_*
