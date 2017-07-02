obj-m += xdma.o

# Path to the Linux kernel, if not passed in as arg, set default.
ifeq ($(KDIR),)
	KDIR := /lib/modules/$(shell uname -r)/build
endif


all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean


# README
# try compiling from x86_64 with the Xilinx SDK:
# $ KDIR=<petalinux-project>/build/linux/kernel/link-to-kernel-build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- make
