CC=gcc

QDMAUTILS_DIR = $(QDMA_DIR)/qdmautils/
QDMAUTILS_OBJ = $(QDMAUTILS_DIR)/{qdmautils.o,dmaxfer.o,qdmactl.o,qdmactl_reg.o}
INCLUDES = -I$(QDMAUTILS_DIR) -I$(QDMA_DIR)/include
LDFLAGS_ = $(LDFLAGS) -laio -pthread


.PHONY: all

all: build

.PHONY: build

build: libxdma.so

libxdma.so: libxdma.o
	$(CC) -g -shared -Wl,-soname,libxdma.so -o libxdma.so libxdma.o $(QDMAUTILS_OBJ) $(LDFLAGS_)

libxdma.o: libxdma.c
	$(CC) $(CFLAGS) libxdma.c $(INCLUDES) -pthread -fPIC -c -g

