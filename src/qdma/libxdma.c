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
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "../libxdma.h"

#define QDMA_DEV_ID      "02000"
#define QDMA_Q_IDX       1
#define QDMA_DEV_ID_ENV  "XDMA_QDMA_DEV"

#define DEV_ALIGN         (512/8) //Buses are 512b wide
#define DEV_MEM_SIZE      0x800000000 ///<Device memory (32GB)
#define DEV_MEM_SIZE_ENV  "XDMA_DEV_MEM_SIZE"

#define MAX_TRANSFER_SIZE  256*1024*1024

int _qdmaFd;

static uintptr_t _curDevMemPtr;

static pthread_mutex_t _allocateMutex;
static pthread_mutex_t _copyMutex;

static void parse_dev_list(const char *devList) {
    //parse device list
    //printf("%s\n", devList);
}

// Get dev mem size from env variable or use the default
static size_t getDeviceMemSize(){
    const char* devMemSize = getenv(DEV_MEM_SIZE_ENV);
    if (!devMemSize)
        return DEV_MEM_SIZE;
    else
        return (size_t) strtoull(devMemSize, NULL, 10);
}

// Get qdma device id from env variable or use the default
static const char *getDeviceId(){
    const char* devId = getenv(QDMA_DEV_ID_ENV);
    if (!devId)
        return QDMA_DEV_ID;
    else
        return devId;
}

xdma_status xdmaInit() {
    //Open files
    char devFileName[24];
    const char* devId = getDeviceId();
    sprintf(devFileName, "/dev/qdma%s-MM-%d", devId, QDMA_Q_IDX);

    _qdmaFd = open(devFileName, O_RDWR);
    if (_qdmaFd < 0) {
        perror("XDMA: ");
        if (errno == ENOENT){
            fprintf(stderr, "%s not found!\n", devFileName);
            fprintf(stderr, "Note: XDMA_QDMA_DEV env variable should contain\
                    the qdma device ID ");
        }
        return XDMA_ERROR;
    }

    //Initialize dummy allocator
    pthread_mutex_init(&_allocateMutex, NULL);
    pthread_mutex_init(&_copyMutex, NULL);
    _curDevMemPtr = 0;

    return XDMA_SUCCESS;
}

xdma_status xdmaFini() {
    //close queue files
    //stop queues
    //delete queues
    if (_qdmaFd > 0) {
        close(_qdmaFd);
    }
    return XDMA_SUCCESS;
}

xdma_status xdmaGetNumDevices(int *numDevices) {
    *numDevices = 1;
    return XDMA_SUCCESS;
}

xdma_status xdmaAllocateHost(int devId, void **buffer, xdma_buf_handle *handle, size_t len) {
    //QDMA does not support memory mapped device buffers
    return XDMA_ENOSYS;
}

xdma_status xdmaAllocate(int devId, xdma_buf_handle *handle, size_t len) {
    void *ptr;
    size_t nlen;

    pthread_mutex_lock(&_allocateMutex);
    nlen = ((len + (DEV_ALIGN + 1))/DEV_ALIGN)*DEV_ALIGN;
    //adjust size so we always get aligned addresses
    if (_curDevMemPtr + nlen > getDeviceMemSize()) {  //_curDevMemPtr starts at 0
        pthread_mutex_unlock(&_allocateMutex);
        return XDMA_ENOMEM;
    }
    ptr = (void*)_curDevMemPtr;
    _curDevMemPtr += nlen;
    pthread_mutex_unlock(&_allocateMutex);

    *handle = ptr;  //Directly store device pointer into handle

    return XDMA_SUCCESS;
}

xdma_status xdmaFree(xdma_buf_handle handle) {
    return XDMA_SUCCESS;    //Do nothing
}

xdma_status xdmaMemcpy(void *usr, xdma_buf_handle buffer, size_t len, size_t offset,
        xdma_dir mode) {
    ssize_t tx;
    size_t transferred = 0, rem = len;
    off_t seekOff;
    off_t devOffset = (off_t)buffer + offset;
    pthread_mutex_lock(&_copyMutex);
    seekOff = lseek(_qdmaFd, devOffset, SEEK_SET);
    if (seekOff != devOffset) {
        if (seekOff < 0) perror("XDMA dev offset:");
        pthread_mutex_unlock(&_copyMutex);
        return XDMA_ERROR;
    }
    if (mode == XDMA_TO_DEVICE) {
        while (transferred < len) {
            int chunkSize = rem < MAX_TRANSFER_SIZE ? rem : MAX_TRANSFER_SIZE;
            lseek(_qdmaFd, devOffset + transferred, SEEK_SET);
            tx = write(_qdmaFd, usr + transferred, chunkSize);
            rem -= tx;
            transferred += tx;
            if (tx < chunkSize) {
                perror("XDMA memcpy chunk error (trying to continue)");
            }
        }
    } else if (mode == XDMA_FROM_DEVICE) {
        while (transferred < len) {
            int chunkSize = rem < MAX_TRANSFER_SIZE ? rem : MAX_TRANSFER_SIZE;
            lseek(_qdmaFd, devOffset + transferred, SEEK_SET);
            tx = read(_qdmaFd, usr + transferred, chunkSize);
            rem -= tx;
            transferred += tx;
            if (tx < chunkSize) {
                perror("XDMA memcpy chunk error (trying to continue)");
            }
        }
    } else {
        pthread_mutex_unlock(&_copyMutex);
        return XDMA_ENOSYS; //Device to device transfers not yet implemented
    }
    pthread_mutex_unlock(&_copyMutex);
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

xdma_status xdmaGetDeviceAddress(xdma_buf_handle buffer, unsigned long *devAddress) {
    *devAddress = (unsigned long)buffer; //a buffer handle is just the device pointer
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
