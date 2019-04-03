# XDMA

This repository contains a set of tools to stream in/out data to/from Xilinx FPGAs.
They are expected to work, at least, in Linux kernel 3.19 and 4.6; without matter if they are 32/64 bits.
The main contents are:
 - `apps` contains different tests/benchmarks to check that the system works.
 - `driver` [deprecated] contains the Linux driver that implements the functionalities.
 - `lib` contains the user level library interfacing the main functionalities.

### Build the driver

> **NOTE: The xdma driver is deprecated and the functionalities have been implemented in the new [OmpSs@FPGA kernel module](https://pm.bsc.es/gitlab/ompss-at-fpga/ompss-at-fpga-kernel-module).**

The build instructions for the xdma linux diver can be found in `driver/README.md` file.

### Build the library
  1. Clone the repository or download the latest stable version.
    ```
    git clone https://pm.bsc.es/gitlab/ompss-at-fpga/xdma.git
    cd xdma
    ```

  2. Enter the library directory.
    ```
    cd lib
    ```

  3. Set environment variables.
    * [Optional] `CROSS_COMPILE`. If you are cross-compiling, set this variable to the right value. For example:
    ```
    export CROSS_COMPILE=arm-linux-gnueabihf-
    ```
    * `KERNEL_MODULE_DIR`. Path where to find the `ompss_fpga.h` header file of [OmpSs@FPGA kernel module](https://pm.bsc.es/gitlab/ompss-at-fpga/ompss-at-fpga-kernel-module). For example:
    ```
    export KERNEL_MODULE_DIR=/path/to/ompss-at-fpga/kernel/module/src
    ```

  4. Build.
    ```
    make
    ```

  5. (Optional) Install the files in `PREFIX` folder. For example:
    ```
    make PREFIX=/opt/install-arm/libxdma install
    ```
