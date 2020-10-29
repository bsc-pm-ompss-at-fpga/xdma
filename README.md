# XDMA

This repository contains a library to stream in/out data to/from FPGAs.
Moreover, it provides an interface to allocate memory for such data transfers.
They are expected to work, at least, in Linux kernel 3.19 and 4.6; without matter if they are 32/64 bits.

### Build the library

The `src` directory contains a folder for each supported platform.
The Makefile inside each platform directory provides the basic targets to build and install the library.
See the README file inside each directory for specific options.

### Build the documentation

A minimal documentation is provided using the `doxygen` tool.
It can be generated with the following commands:

```
cd src
make doc
```
