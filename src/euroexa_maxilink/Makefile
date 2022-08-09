CC = ${CROSS_COMPILE}gcc
LDFLAGS := -lxdma
CFLAGS := -c -Wall -Werror -std=c99 -g
INCLUDES := -I. -I$(KERNEL_MODULE_DIR)

.PHONY: all
all: build

.PHONY: build
build: libxdma.so libxdma-static.a

libxdma.o : libxdma.c ../libxdma.h
	$(CC) $(CFLAGS) $(INCLUDES) -fpic libxdma.c


libxdma.so : libxdma.o
	$(CC) -shared -Wl,-soname,libxdma.so -o libxdma.so libxdma.o

libxdma-static.a : libxdma.o
	ar rcs $@ $^

.PHONY: ../libxdma_version.h
libxdma_version.h: ../libxdma_version_template.h
	@awk '1;/#define LIBXDMA_VERSION_MINOR/{exit}' $^ >$@
ifeq (x$(shell git rev-parse --is-bare-repository 2>/dev/null), xfalse)
	@echo "/* Build commit" >>$@
	git show -s >>$@
	@echo "*/" >>$@
	@echo "#define LIBXDMA_VERSION_COMMIT \\" >>$@
	@git show -s --format=%H >>$@
	@echo "" >>$@
	@echo "/* Build branch and status" >>$@
	git status -b -s >>$@
	@echo "*/" >>$@
else
	@echo "#define LIBXDMA_VERSION_COMMIT \\" >>$@
	@echo "unknown" >>$@
endif
	@tail -n 2 $^ >>$@

install: libxdma.so libxdma-static.a ../libxdma.h libxdma_version.h
	mkdir -p $(PREFIX)/lib
	cp libxdma.so libxdma-static.a $(PREFIX)/lib/
	mkdir -p $(PREFIX)/include
	cp ../libxdma.h libxdma_version.h $(PREFIX)/include/

.PHONY: clean
clean:
	rm -f *.o libxdma.so libxdma-static.a libxdma_version.h
	rm -rf html latex
