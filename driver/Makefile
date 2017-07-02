obj-m += xdma.o

# Path to the Linux kernel, if not passed in as arg, set default.
ifeq ($(KDIR),)
	KDIR := /lib/modules/$(shell uname -r)/build
endif


all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

install: xdma.ko
	@echo Copying xdma rules to /etc/udev/rules.d
	sudo cp 80-xdma.rules /etc/udev/rules.d/
	@echo Copying xdma module to /lib/modules/$$(uname -r)/extra
	if [ ! -d /lib/modules/$$(uname -r)/extra ] ; then \
	   sudo mkdir /lib/modules/$$(uname -r)/extra ; \
	fi 
	sudo cp xdma.ko /lib/modules/$$(uname -r)/extra
	@echo Determining new module dependences
	sudo depmod -a

# README
# try compiling from x86_64 with the Xilinx SDK:
# $ KDIR=<petalinux-project>/build/linux/kernel/link-to-kernel-build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- make
