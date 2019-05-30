CXX   ?= g++
CXX_   = $(CROSS_COMPILE)$(CXX)

CXXFLAGS_    = $(CXXFLAGS) -Wall -Werror -fpic
LDFLAGS_     = $(LDFLAGS)
TARGETS_     =

## Check libxdma installation
#LIBXDMA_DIR      ?= /opt/install-arm/libxdma
LIBXDMA_INC_DIR  ?= $(LIBXDMA_DIR)/include
LIBXDMA_LIB_DIR  ?= $(LIBXDMA_DIR)/lib
CXXFLAGS_        += -I$(LIBXDMA_INC_DIR)
LDFLAGS_         += -L$(LIBXDMA_LIB_DIR) -lxdma

all: xdma-num-devs

xdma-num-devs.o: xdma-num-devs.cpp
	$(CXX_) $(CXXFLAGS_) -c $^

xdma-num-devs: xdma-num-devs.o
	$(CXX_) -Wl,-rpath=$(LIBXDMA_LIB_DIR) -o $@ $^ $(LDFLAGS_)

clean:
	rm -f *.o xdma-num-devs
