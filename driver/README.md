# xdma Linux driver

> **NOTE: This driver is deprecated and the functionalities have been implemented in the new [OmpSs@FPGA kernel module](https://pm.bsc.es/gitlab/ompss-at-fpga/ompss-at-fpga-kernel-module).**

### Build the driver

To build the driver, you need the kernel headers or sources of your revision.
In a Debian based system, you can install the kernel header files for the currently running kernel by running the following in a terminal:
```
sudo apt-get install linux-headers-$(uname -r)
```

Once you have the kernel headers available, you can proceed with:

  1. Clone the repository or download the latest stable version.
    ```
    git clone https://pm.bsc.es/gitlab/ompss-at-fpga/xdma.git
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
