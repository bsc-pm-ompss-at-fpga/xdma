#include <iostream>
#include <string>

#include "libxdma.h"

void assert(const xdma_status stat, const std::string err) {
    if (stat != XDMA_SUCCESS) {
        std::cerr << "ERROR: " << err << std::endl;
        exit(1);
    }
}

int main() {
    xdma_status s;
    s = xdmaOpen();
    assert(s, "xdmaOpen failed");

    int numDevs;
    s = xdmaGetNumDevices(&numDevs);
    assert(s, "xdmaGetNumDevices failed");
    std::cout << numDevs << std::endl;

    s = xdmaClose();
    assert(s, "xdmaClose failed");
}
