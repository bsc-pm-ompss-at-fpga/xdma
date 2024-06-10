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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "../util/ticket-lock.h"

#include "../libxdma.h"

#define QDMA_Q_IDX       1
#define QDMA_DEV_ID_ENV  "XDMA_QDMA_DEV"

#define DEV_ALIGN         (512/8) //Buses are 512b wide
#define DEV_MEM_SIZE      0x800000000 ///<Device memory (32GB)
#define DEV_MEM_SIZE_ENV  "XDMA_DEV_MEM_SIZE"

//For some reason, QDMA 5 from vivado 2022.2 fails when using larger chunks
#define MAX_TRANSFER_SIZE 1*1024*1024
#define MAX_DEVICES 16

static int _ndevs = 0;
static int _qdmaFd[MAX_DEVICES];

static uintptr_t _curDevMemPtr[MAX_DEVICES];

static ticketLock_t _copyMutex[MAX_DEVICES];

// Internal library representation of an alloc
typedef struct {
    int devId;
    uint64_t devPtr;
} alloc_info_t;


// Get dev mem size from env variable or use the default
static size_t getDeviceMemSize(){
    const char* devMemSize = getenv(DEV_MEM_SIZE_ENV);
    if (!devMemSize)
        return DEV_MEM_SIZE;
    else
        return (size_t) strtoull(devMemSize, NULL, 10);
}

// Get qdma device id from env variable
static const char *getDeviceList() {
    const char* devIdList = getenv(QDMA_DEV_ID_ENV);
    if (devIdList == NULL) {
        fprintf(stderr, "[XDMA] Environment variable " QDMA_DEV_ID_ENV " not set, it should contain the QDMA device ID list\n");
    }
    return devIdList;
}

xdma_status xdmaInit() {
    //Open files
    const char* devListEnv = getDeviceList();
    if (devListEnv == NULL) {
        return XDMA_ERROR;
    }
    int ndevs = 0;
    _ndevs = 0;
    char *devId;
    char *devList = malloc(strlen(devListEnv)+1);
    strcpy(devList, devListEnv);
    devId = strtok(devList, " ");
    while (devId != NULL) {
        if (ndevs == MAX_DEVICES) {
            fprintf(stderr, "Found too many devices\n");
            goto init_maxdev_err;
        }

        char devFileName[24];
        sprintf(devFileName, "/dev/qdma%s-MM-%d", devId, QDMA_Q_IDX);

        _qdmaFd[ndevs] = open(devFileName, O_RDWR);
        if (_qdmaFd[ndevs] < 0) {
            perror("XDMA open error");
            if (errno == ENOENT) {
                fprintf(stderr, "%s not found!\n", devFileName);
            }
            goto init_open_err;
        }

        fprintf(stderr, "[XDMA] Found device %s\n", devId);

        ++ndevs;
        devId = strtok(NULL, " ");
    }
    free(devList);

    for (int i = 0; i < ndevs; ++i) {
        //Initialize dummy allocator
        ticketLockInit(&_copyMutex[i]);
        _curDevMemPtr[i] = 0;
    }
    _ndevs = ndevs;

    return XDMA_SUCCESS;

init_maxdev_err:
init_open_err:
    for (int d = 0; d < ndevs; ++d)
        close(_qdmaFd[d]);
    free(devList);

    return XDMA_ERROR;
}

xdma_status xdmaFini() {
    //close queue files
    //stop queues
    //delete queues
    for (int i = 0; i < _ndevs; ++i) {
        if (_qdmaFd[i] > 0) {
            close(_qdmaFd[i]);
        }
    }
    return XDMA_SUCCESS;
}

xdma_status xdmaGetNumDevices(int *numDevices) {
    *numDevices = _ndevs;
    return XDMA_SUCCESS;
}

xdma_status xdmaAllocateHost(int devId, void **buffer, xdma_buf_handle *handle, size_t len) {
    //QDMA does not support memory mapped device buffers
    return XDMA_ENOSYS;
}

xdma_status xdmaAllocate(int devId, xdma_buf_handle *handle, size_t len) {
    uint64_t nlen = ((len + (DEV_ALIGN + 1))/DEV_ALIGN)*DEV_ALIGN;
    uint64_t ptr = __atomic_fetch_add(_curDevMemPtr + devId, nlen, __ATOMIC_RELAXED);
    //adjust size so we always get aligned addresses
    if (ptr + nlen > getDeviceMemSize()) {  //_curDevMemPtr starts at 0
        return XDMA_ENOMEM;
    }

    alloc_info_t* alloc_info = (alloc_info_t*)malloc(sizeof(alloc_info_t));
    alloc_info->devPtr = ptr;
    alloc_info->devId = devId;

    *handle = alloc_info;  //Directly store device pointer into handle

    return XDMA_SUCCESS;
}

xdma_status xdmaFree(xdma_buf_handle handle) {
    free((alloc_info_t*)handle);
    return XDMA_SUCCESS;    //Do nothing
}

xdma_status xdmaMemcpy(void *usr, xdma_buf_handle handle, size_t len, size_t offset,
        xdma_dir mode) {
    ssize_t tx;
    size_t transferred = 0, rem = len;
    alloc_info_t* alloc_info = (alloc_info_t*)handle;

    int devId = alloc_info->devId;
    uint64_t buffer = alloc_info->devPtr;

    off_t seekOff;
    off_t devOffset = (off_t)buffer + offset;
    ticketLockAcquire(&_copyMutex[devId]);
    seekOff = lseek(_qdmaFd[devId], devOffset, SEEK_SET);
    if (seekOff != devOffset) {
        if (seekOff < 0) perror("XDMA dev offset:");
        ticketLockRelease(&_copyMutex[devId]);
        return XDMA_ERROR;
    }
    if (mode == XDMA_TO_DEVICE) {
        while (transferred < len) {
            int chunkSize = rem < MAX_TRANSFER_SIZE ? rem : MAX_TRANSFER_SIZE;
            lseek(_qdmaFd[devId], devOffset + transferred, SEEK_SET);
            tx = write(_qdmaFd[devId], usr + transferred, chunkSize);
            rem -= tx;
            transferred += tx;
            if (tx < chunkSize) {
                perror("XDMA memcpy chunk error (trying to continue)");
            }
        }
    } else if (mode == XDMA_FROM_DEVICE) {
        while (transferred < len) {
            int chunkSize = rem < MAX_TRANSFER_SIZE ? rem : MAX_TRANSFER_SIZE;
            lseek(_qdmaFd[devId], devOffset + transferred, SEEK_SET);
            tx = read(_qdmaFd[devId], usr + transferred, chunkSize);
            rem -= tx;
            transferred += tx;
            if (tx < chunkSize) {
                perror("XDMA memcpy chunk error (trying to continue)");
            }
        }
    } else {
        ticketLockRelease(&_copyMutex[devId]);
        return XDMA_ENOSYS; //Device to device transfers not yet implemented
    }
    ticketLockRelease(&_copyMutex[devId]);
    if (transferred != len) {
        perror("XDMA memcpy error");
        return XDMA_ERROR;
    } else {
        return XDMA_SUCCESS;
    }
}

xdma_status xdmaMemcpyAsync(void *usr, xdma_buf_handle buffer, size_t len, size_t offset,
        xdma_dir mode, xdma_transfer_handle *transfer) {
    *transfer = 0;
    return xdmaMemcpy(usr, buffer, len, offset, mode);
}

xdma_status xdmaTestTransfer(xdma_transfer_handle *transfer) {
    return XDMA_SUCCESS;
}

xdma_status xdmaWaitTransfer(xdma_transfer_handle *transfer) {
    return XDMA_SUCCESS;
}

xdma_status xdmaGetDeviceAddress(xdma_buf_handle handle, unsigned long *devAddress) {
    *devAddress = (unsigned long)((alloc_info_t*)handle)->devPtr;
    return XDMA_SUCCESS;
}

xdma_status xdmaInitHWInstrumentation() {
    return XDMA_ENOSYS;
}

xdma_status xdmaFiniHWInstrumentation() {
    return XDMA_ENOSYS;
}

xdma_status xdmaGetDeviceTime(int devId, uint64_t *time) {
    return XDMA_ENOSYS;
}

int xdmaInstrumentationEnabled() {
    return 0;
}

uint64_t xdmaGetInstrumentationTimerAddr(int devId) {
    return XDMA_ENOSYS;
}
