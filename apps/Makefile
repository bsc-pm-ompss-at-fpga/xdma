CC=${CROSS_COMPILE}gcc
CFLAGS=-std=gnu99 -Wall -g -O0

CXX=$(CROSS_COMPILE)g++
CXXFLAGS=-Wall -g -O0

LDFLAGS=-L../lib -lxdma
INCLUDES=-I. -I../lib

FPGACC=$(CROSS_COMPILE)fpgacc
FPGACFLAGS=--ompss -std=c99 -k --debug

EXECUTABLE = \
	loopback \
	gen_task \
	matmul


.PHONY : all
all: $(EXECUTABLE)

loopback: loopback.c
	$(CC) $(INCLUDES) $(CFLAGS) $< -o $@ $(LDFLAGS)

gen_task: gen_task.cpp
	$(CXX) $(INCLUDES) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

matmul: matmul.c
	$(CC) $(INCLUDES) $(CFLAGS) -Wno-format-security $< -o $@ $(LDFLAGS)

gen_task_thread: gen_task_thread.cpp
	$(CXX) $(INCLUDES) $(CXXFLAGS) $< -pthread -o $@ $(LDFLAGS)


loopback_ompss: loopback_ompss.c
	$(FPGACC) $(FPGACFLAGS) $< -o $@

gen-task_ompss: gen-task_ompss.c
	$(FPGACC) $(FPGACFLAGS) $< -o $@

matmul_ompss: matmul_ompss.c
	$(FPGACC) $(FPGACFLAGS) $< -o $@


.PHONY : clean
clean:
	rm -f $(EXECUTABLE) hls_*
