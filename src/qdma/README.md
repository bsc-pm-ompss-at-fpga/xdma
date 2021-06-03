# Build libxdma for Xilinx Zynq platform

### Prerequisites

 - Xilinx qdma drivers from [Xilinx's dma ip driver github](https://github.com/Xilinx/dma_ip_drivers)

### Instructions

  1. Clone the repository or download the latest stable version.
    ```
    git clone https://gitlab.bsc.es/ompss-at-fpga/xdma.git
    cd xdma/src/qdma
    ```

  2. Build.
    ```
    make
    ```

  3. (Optional) Install the files in `PREFIX` folder. For example:
    ```
    make PREFIX=/opt/install-arm/libxdma install
    ```
