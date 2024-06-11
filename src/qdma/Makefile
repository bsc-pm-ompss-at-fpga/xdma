CC = ${CROSS_COMPILE}gcc

LDFLAGS_ = $(LDFLAGS)

.PHONY: all

all: build

.PHONY: build

build: libxdma.so

libxdma.so: libxdma.o
	$(CC) -g -shared -Wl,-soname,libxdma.so -o libxdma.so libxdma.o $(LDFLAGS_)

libxdma.o: libxdma.c
	$(CC) $(CFLAGS) libxdma.c -pthread -fPIC -c -g -O3

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

install: libxdma.so ../libxdma.h libxdma_version.h
	mkdir -p $(PREFIX)/lib
	cp libxdma.so $(PREFIX)/lib/
	mkdir -p $(PREFIX)/include
	cp ../libxdma.h libxdma_version.h $(PREFIX)/include/

.PHONY: clean
clean:
	rm -f *.o libxdma.so libxdma-static.a libxdma_version.h
	rm -rf html latex
