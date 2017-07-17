# XDMA

This repository contains a set of tools to stream in/out data to/from Xilinx FPGAs.
They are expected to work, at least, in Linux kernel 3.19 and 4.6; without matter if they are 32/64 bits.
The main contents are:
 - `apps` contains different tests/benchmarks to check that the system works.
 - `driver` contains the Linux driver that implements the functionalities.
 - `lib` contains the user level library interfacing the main functionalities.

### Build the driver

To build the driver, you need the kernel headers or sources of your revision.
In a Debian based system, you can install the kernel header files for the currently running kernel by running the following in a terminal:
```
sudo apt-get install linux-headers-$(uname -r)
```

Once you have the kernel headers available, you can proceed with:

  1. Clone the repository or download the latest stable version.
    ```
    git clone https://pm.bsc.es/gitlab/afilguer/xdma.git
    cd xdma
    ```

  2. Enter the driver directory.
    ```
    cd driver
    ```

  3. (Optional) If you are cross-compiling, set `KDIR`, `CROSS_COMPILE` and `ARCH` environment variables.
    * `KDIR` should point to the folder containing the kernel headers.
    Otherwise, they are expected to be in `/lib/modules/$(shell uname -r)/build` folder.
    ```
    export KDIR=/home/my_user/kernel-headers
    ```
    * `ARCH` should be set to the target architecture you're compiling to.
    ```
    export ARCH=arm
    ```
    * `CROSS_COMPILE` must contain the build triplet for your target system.
    ```
    export CROSS_COMPILE=arm-linux-gnueabihf-
    ```
    
  4. Build the kernel module.
    ```
    make
    ```
    
  5. (Optional) Install the kernel module and udev rules.
    ```
    make install
    ```


### Build the library
  1. Clone the repository or download the latest stable version.
    ```
    git clone https://pm.bsc.es/gitlab/afilguer/xdma.git
    cd xdma
    ```
    
  2. Enter the library directory.
    ```
    cd lib
    ```
    
  3. (Optional) If you are cross-compiling, set the `CROSS_COMPILE` environment variable. For example:
    ```
    export CROSS_COMPILE=arm-linux-gnueabihf-
    ```
     
  4. Build.
    ```
    make
    ```
    
  5. (Optional) Install the files in `PREFIX` folder. For example:
    ```
    make PREFIX=/opt/install-arm/libxdma install
    ```
