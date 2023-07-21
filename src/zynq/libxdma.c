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

#include "../libxdma.h"

#define INSTR_FILEPATH  "/dev/ompss_fpga/hwcounter"
#define MEM_FILEPATH    "/dev/ompss_fpga/xdma_mem"
#define MAP_SIZE  (33554432)
#define FILESIZE (MAP_SIZE * sizeof(uint8_t))

#include "ompss_fpga.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <alloca.h>
#include <pthread.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>

#define BUS_IN_BYTES        4
#define BUS_BURST           16
#define MAX_DEVICES         5
//Assume there only are 1 in + 1 out channel per device
#define CHANNELS_PER_DEVICE 2
#define MAX_CHANNELS        MAX_DEVICES*CHANNELS_PER_DEVICE

static int _fd;
static int _instr_fd = 0;

static pthread_mutex_t _allocateMutex;
static int _open_cnt = 0;

typedef enum {
    ALLOC_HOST, ALLOC_DEVICE
} alloc_type_t;

// Internal library representation of an alloc
typedef struct {
    alloc_type_t type;      ///< Type of allocation
    unsigned long handle;   ///< Device driver handle
    void * ptr;             ///< Map pointer in userspace
    unsigned long devAddr;  ///< Buffer device address
} alloc_info_t;

xdma_status xdmaInit() {
    int open_cnt;

    // Handle multiple opens
    open_cnt = __sync_fetch_and_add(&_open_cnt, 1);
    if (open_cnt > 0) return XDMA_SUCCESS;

    _instr_fd = 0;

    _fd = open(MEM_FILEPATH, O_RDWR);
    if (_fd == -1) {
        xdma_status ret = XDMA_ERROR;
        switch (errno) {
            case EACCES: //User cannot access instrumentation device
                ret = XDMA_EACCES;
                break;
            case ENOENT: //Instrumentation not available
                ret = XDMA_ENOENT;
                break;
        }
        return ret;
    }

    //Initialize mutex
    pthread_mutex_init(&_allocateMutex, NULL);

    return XDMA_SUCCESS;
}

xdma_status xdmaFini() {
    int open_cnt;

    // Handle multiple opens
    open_cnt = __sync_sub_and_fetch(&_open_cnt, 1);
    if (open_cnt > 0) return XDMA_SUCCESS;

    if (_fd > 0) {
        close(_fd);
    }
    //Mutex finalization
    pthread_mutex_destroy(&_allocateMutex);

    return XDMA_SUCCESS;
}

xdma_status xdmaGetNumDevices(int *numDevices){
    *numDevices = 1;
    return XDMA_SUCCESS;
}

xdma_status _getDeviceAddress(unsigned long dmaBuffer, unsigned long *address) {
    union {
        unsigned long dmaBuffer;
        unsigned long dmaAddress;
    } kArg;
    int status;
    kArg.dmaBuffer = dmaBuffer;
    status = ioctl(_fd, XDMAMEM_GET_DMA_ADDRESS, &kArg);
    if (status) {
        perror("Error getting DMA address");
        return XDMA_ERROR;
    } else {
        *address = kArg.dmaAddress;
        return XDMA_SUCCESS;
    }
}

xdma_status xdmaAllocateHost(int devId, void **buffer, xdma_buf_handle *handle, size_t len) {
    //TODO: Check that mmap + ioctl are performet atomically
    unsigned long ret, devAddress;
    unsigned int status;
    pthread_mutex_lock(&_allocateMutex);
    *buffer = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, _fd, 0);
    if (*buffer == MAP_FAILED) {
        pthread_mutex_unlock(&_allocateMutex);
        return XDMA_ENOMEM;
    }
    //get the handle for the allocated buffer
    status = ioctl(_fd, XDMAMEM_GET_LAST_KBUF, &ret);
    pthread_mutex_unlock(&_allocateMutex);
    if (status) {
        perror("Error allocating pinned memory");
        munmap(buffer, len);
        return XDMA_ERROR;
    }

    status = _getDeviceAddress(ret, &devAddress);

    alloc_info_t *info = (alloc_info_t *)malloc(sizeof(alloc_info_t));
    info->type = ALLOC_HOST;
    info->handle = ret;
    info->ptr = *buffer;
    info->devAddr = devAddress;
    *handle = (xdma_buf_handle)info;
    return status;
}

xdma_status xdmaAllocate(int devId, xdma_buf_handle *handle, size_t len) {
    void *ptr;
    alloc_info_t *info;
    xdma_status stat = xdmaAllocateHost(devId, &ptr, (xdma_buf_handle)&info, len);
    if (stat == XDMA_SUCCESS) {
        info->type = ALLOC_DEVICE;
        *handle = (xdma_buf_handle)info;
    }
    return stat;
}

xdma_status xdmaFree(xdma_buf_handle handle) {
    if (handle == NULL) {
        return XDMA_EINVAL;
    }
    alloc_info_t *info = (alloc_info_t *)handle;
    int size;
    xdma_status ret = XDMA_SUCCESS;
    size = ioctl(_fd, XDMAMEM_RELEASE_KBUF, &info->handle);
    if (size <= 0) {
        perror("Could not release pinned buffer");
        ret = XDMA_ERROR;
    } else if (munmap(info->ptr, size)) {
        perror("Failed to unmap pinned buffer");
        ret = XDMA_ERROR;
    }
    free(info);
    return ret;
}

xdma_status xdmaMemcpy(void *usr, xdma_buf_handle buffer, size_t len, size_t offset,
        xdma_dir mode)
{
    alloc_info_t *info = (alloc_info_t *)buffer;
    xdma_status ret = XDMA_SUCCESS;
    if (mode == XDMA_TO_DEVICE) {
        void *src, *dst;
        src = usr;
        dst = ((unsigned char*)info->ptr + offset);

        // long words for aligned accesses
        // using these pointers as 64b casters for the original pointers
        uint64_t **lsrc, **ldst;
        lsrc = (uint64_t**)&src;
        ldst = (uint64_t**)&dst;

        // byte words for unaligned accesses
        // using these pointers as 8b casters for the original pointers
        uint8_t **bsrc, **bdst;
        bsrc = (uint8_t**)&src;
        bdst = (uint8_t**)&dst;

        // prologue for unaligned accesses
        // copy data byte by byte until buffer is aligned
        while ((uintptr_t)dst % sizeof(uint64_t)) {
            **bdst = **bsrc;
            (*bsrc)++;
            (*bdst)++;
            len--;
        }

        // copy aligned data
        while (len/sizeof(uint64_t)) {
            **ldst = **lsrc;
            (*ldst)++;
            (*lsrc)++;
            len -= sizeof(uint64_t);
        }

        // epilogue for remaining unaligned accesses
        // copy remaining len data byte by byte
        while (len%sizeof(uint64_t)) {
            **bdst = **bsrc;
            (*bsrc)++;
            (*bdst)++;
            len--;
        }
    } else if (mode == XDMA_FROM_DEVICE) {
        void *src, *dst;
        src = ((unsigned char*)info->ptr + offset);
        dst = usr;

        // long words for aligned accesses
        // using these pointers as 64b casters for the original pointers
        uint64_t **lsrc, **ldst;
        lsrc = (uint64_t**)&src;
        ldst = (uint64_t**)&dst;

        // byte words for unaligned accesses
        // using these pointers as 8b casters for the original pointers
        uint8_t **bsrc, **bdst;
        bsrc = (uint8_t**)&src;
        bdst = (uint8_t**)&dst;

        // prologue for unaligned accesses
        // copy data byte by byte until buffer is aligned
        while ((uintptr_t)src % sizeof(uint64_t)) {
            **bdst = **bsrc;
            (*bsrc)++;
            (*bdst)++;
            len--;
        }

        // copy aligned data
        while (len/sizeof(uint64_t)) {
            **ldst = **lsrc;
            (*ldst)++;
            (*lsrc)++;
            len -= sizeof(uint64_t);
        }

        // epilogue for remaining unaligned accesses
        // copy len data byte by byte
        while (len%sizeof(uint64_t)) {
            **bdst = **bsrc;
            (*bsrc)++;
            (*bdst)++;
            len--;
        }
    } else if (mode == XDMA_DEVICE_TO_DEVICE) {
        ret = XDMA_ENOSYS;
    } else {
        ret = XDMA_EINVAL;
    }
    return ret;
}

xdma_status xdmaMemcpyAsync(void *usr, xdma_buf_handle buffer, size_t len, size_t offset,
        xdma_dir mode, xdma_transfer_handle *transfer)
{
    *transfer = (xdma_transfer_handle)NULL;
    return xdmaMemcpy(usr, buffer, len, offset, mode);
}

xdma_status xdmaTestTransfer(xdma_transfer_handle *transfer){
    return XDMA_SUCCESS;
}

xdma_status xdmaWaitTransfer(xdma_transfer_handle *transfer){
    return XDMA_SUCCESS;
}

xdma_status xdmaGetDeviceAddress(xdma_buf_handle buffer, unsigned long *address) {
    alloc_info_t *info = (alloc_info_t *)buffer;
    *address = info->devAddr;
    return XDMA_SUCCESS;
}

xdma_status xdmaInitHWInstrumentation() {
    if (_instr_fd > 0) return XDMA_SUCCESS;

    //Open instrumentation file
    _instr_fd = open(INSTR_FILEPATH, O_RDONLY);
    if (_instr_fd == -1) {
        //perror("Error opening instrumentation device file for reading");
        xdma_status ret = XDMA_ERROR;
        switch (errno) {
            case EACCES: //User cannot access instrumentation device
                ret = XDMA_EACCES;
                break;
            case ENOENT: //Instrumentation not available
                ret = XDMA_ENOENT;
                break;
        }
        return ret;
    }
    return XDMA_SUCCESS;
}

xdma_status xdmaFiniHWInstrumentation() {
    if (_instr_fd > 0) {
        close(_instr_fd);
    }
    return XDMA_SUCCESS;
}

xdma_status xdmaGetDeviceTime(int devId, uint64_t *time) {
    if (_instr_fd <= 0) return XDMA_ENOENT;

    int rd;
    rd = read(_instr_fd, time, sizeof(uint64_t));
    //FIXME: clear high 32bit until HW actually returns 64 bit timestamps
    //*time = *time & 0xFFFFFFFFULL;
    //fprintf(stderr, "XDMA_TIME: %llu\n", *time);
    if (rd != sizeof(uint64_t)) {
        return XDMA_ERROR;
    } else {
        return XDMA_SUCCESS;
    }
}

int xdmaInstrumentationEnabled() {
    return (_instr_fd > 0);
}

uint64_t xdmaGetInstrumentationTimerAddr(int devid) {
    if (_instr_fd <= 0) return 0;

    int status;
    uint64_t addr;
    status = ioctl(_instr_fd, HWCOUNTER_GET_ADDR, &addr);
    if (status) {
        perror("Could not get instrumentation address");
        return 0;
    }
    return addr;
}

#undef INSTR_FILEPATH
#undef MAP_SIZE
#undef FILESIZE
