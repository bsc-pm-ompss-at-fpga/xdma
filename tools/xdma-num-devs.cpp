/*--------------------------------------------------------------------
  (C) Copyright 2017-2020 Barcelona Supercomputing Center
                          Centro Nacional de Supercomputacion

  This file is part of OmpSs@FPGA toolchain.

  This code is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 3 of
  the License, or (at your option) any later version.

  OmpSs@FPGA toolchain is distributed in the hope that it will be
  useful, but WITHOUT ANY WARRANTY; without even the implied
  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this code. If not, see <www.gnu.org/licenses/>.
--------------------------------------------------------------------*/

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
