CC = ${CROSS_COMPILE}gcc
LDFLAGS := -lxdma
CFLAGS := -c -Wall -Werror -std=c99 -g
INCLUDES := -I. -I../driver

.PHONY : all
all : libxdma.so libxdma-static.a


libxdma.o : libxdma.c libxdma.h
	$(CC) $(CFLAGS) $(INCLUDES) -fpic libxdma.c


libxdma.so : libxdma.o
	$(CC) -shared -Wl,-soname,libxdma.so -o libxdma.so libxdma.o

libxdma-static.a : libxdma.o
	ar rcs $@ $^

doc:
	doxygen

.PHONY: libxdma_version.h
libxdma_version.h:
	@echo "#ifndef __LIBXDMA_VERSION_H__" >libxdma_version.h
	@echo "#define __LIBXDMA_VERSION_H__" >>libxdma_version.h
	@echo "" >>libxdma_version.h
	@echo "/* Build commit" >>libxdma_version.h
	git show -s >>libxdma_version.h
	@echo "*/" >>libxdma_version.h
	@echo "#define XDMA_COMMIT_INFO \\" >>libxdma_version.h
	@git show -s --format=%H >>libxdma_version.h
	@echo "" >>libxdma_version.h
	@echo "/* Build branch and status" >>libxdma_version.h
	git status -b -s >>libxdma_version.h
	@echo "*/" >>libxdma_version.h
	@echo "" >>libxdma_version.h
	@echo "#endif" >>libxdma_version.h

install: libxdma.so libxdma-static.a libxdma.h libxdma_version.h
	mkdir -p $(PREFIX)/lib
	cp libxdma.so libxdma-static.a $(PREFIX)/lib/
	mkdir -p $(PREFIX)/include
	cp libxdma.h libxdma_version.h $(PREFIX)/include/

.PHONY : clean
clean :
	rm -f *.o libxdma.so libxdma-static.a libxdma_version.h
	rm -rf html latex
