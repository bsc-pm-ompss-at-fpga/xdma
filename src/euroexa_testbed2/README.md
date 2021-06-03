# Build libxdma for Xilinx Zynq platform

### Prerequisites

 - `ompss_fpga.h` header file from [OmpSs@FPGA kernel module](https://gitlab.bsc.es/ompss-at-fpga/ompss-at-fpga-kernel-module).

### Instructions

  1. Clone the repository or download the latest stable version.
    ```
    git clone https://gitlab.bsc.es/ompss-at-fpga/xdma.git
    cd xdma/src/euroexa_testbed2
    ```

  2. Set environment variables.
    * [Optional] `CROSS_COMPILE`. If you are cross-compiling, set this variable to the right value. For example:
    ```
    export CROSS_COMPILE=arm-linux-gnueabihf-
    ```
    * `KERNEL_MODULE_DIR`. Path where to find the `ompss_fpga.h` header file of [OmpSs@FPGA kernel module](https://gitlab.bsc.es/ompss-at-fpga/ompss-at-fpga-kernel-module). For example:
    ```
    export KERNEL_MODULE_DIR=/path/to/ompss-at-fpga/kernel/module/src
    ```

  3. Build.
    ```
    make
    ```

  4. (Optional) Install the files in `PREFIX` folder. For example:
    ```
    make PREFIX=/opt/install-arm/libxdma install
    ```
