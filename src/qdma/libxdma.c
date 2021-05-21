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

int _qdmaFd;

static uintptr_t _devMem;
static uintptr_t _curDevMemPtr;

static pthread_mutex_t _allocateMutex;
static pthread_mutex_t _copyMutex;

static void parse_dev_list(const char *devList) {
    //parse device list
    //printf("%s\n", devList);
}

// Get dev mem size from env variable or use the default
static const size_t getDeviceMemSize(){
    const char* devMemSize = getenv(DEV_MEM_SIZE_ENV);
    if (!devMemSize)
        return DEV_MEM_SIZE;
    else
        return devMemSize;
}

// Get qdma device id from env variable or use the default
static const char *getDeviceId(){
    const char* devId = getenv(QDMA_DEV_ID_ENV);
    if (!devId)
        return QDMA_DEV_ID;
    else
        return devId;
}

xdma_status xdmaOpen() {

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
    xdmaInitMem();

    //TODO: Cleanup in case of error

}

xdma_status xdmaClose() {

    //close queue files
    //stop queues
    //delete queues
}

xdma_status xdmaGetNumDevices(int *numDevices) {

    //Only used for stream devices
    //TODO: Return the number of devices if stream support is enabled
    *numDevices = 0;
    return XDMA_SUCCESS;
}

xdma_status xdmaGetDevices(int entries, xdma_device *devices, int *devs) {
    //Only used for streams
    *devs = 0;

    //TODO: Return the number of devices if stream is enabled
    return XDMA_SUCCESS;
}

xdma_status xdmaGetDeviceChannel(xdma_device device, xdma_dir direction, xdma_channel *channel) {
    //No stream support
    return XDMA_ENOSYS;
}

xdma_status xdmaAllocateHost(void **buffer, xdma_buf_handle *handle, size_t len) {
    //QDMA does not support memory mapped device buffers
    return XDMA_ENOSYS;
}

xdma_status xdmaAllocate(xdma_buf_handle *handle, size_t len) {
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

xdma_status xdmaStream(xdma_buf_handle buffer, size_t len, size_t offset,
        xdma_device dev, xdma_channel channel) {
    return XDMA_ENOSYS;
}

xdma_status xdmaStreamAsync(xdma_buf_handle buffer, size_t len, size_t offset,
        xdma_device dev, xdma_channel channel, xdma_transfer_handle *transfer) {
    return XDMA_ENOSYS;
}

/*!
 * Copy to/from a buffer from/to userspace memory
 *
 * \param[in] usr       Pointer to userspace memory
 * \param[in] buffer    Buffer handle
 * \param[in] len       Length of the data movement (in bytes)
 * \param[in] offset    Offset to apply in the buffer
 * \param[in] mode      Directionality of the data movement
 *                       - XDMA_TO_DEVICE:   buffer[offset .. offset+len] = usr[0 .. len]
 *                       - XDMA_FROM_DEVICE: usr[0 .. len] = buffer[offset .. offset+len]
 * \param[out] transfer Pointer to the variable that will hold the transfer handle.
 *                      (Only available in async version)
 * NOTE: An async operation must be synchronized at some point using xdmaTestTransfer
 *       or xdmaWaitTransfer. Otherwise, the execution may have memory leaks or even
 *       hang
 */
xdma_status xdmaMemcpy(void *usr, xdma_buf_handle buffer, size_t len, size_t offset,
        xdma_dir mode) {
    ssize_t tx;
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
        tx = write(_qdmaFd, usr, len);
    } else if (mode == XDMA_FROM_DEVICE) {
        tx = read(_qdmaFd, usr, len);
    } else {
        pthread_mutex_unlock(&_copyMutex);
        return XDMA_ENOSYS; //Device to device transfers not yet implemented
    }
    pthread_mutex_unlock(&_copyMutex);
    if (tx != len) {
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

/*!
 * Initialize the support for HW instrumentation.
 * Note that the function will fail if the HW instrumentation support is not available in the loaded
 * bitstream.
 * \return  XDMA_SUCCESS  if the support is successfully initialized
 *          XDMA_EISINIT  if the support is already initialized
 *          XDMA_ERROR    otherwise
 */
xdma_status xdmaInitHWInstrumentation();

/*!
 * Finalize the support for HW instrumentation
 * \return  XDMA_SUCCESS  if the support is successfully finalized
 *          XDMA_ERROR    otherwise
 */
xdma_status xdmaFiniHWInstrumentation();

/*!
 * Get the internal device time (instrumentation timer)
 * \param[out] time Current timestamp in the device
 * \return          XDMA_SUCCESS if the time has been successfully set,
 *                  XDMA_ERROR otherwise
 */
xdma_status xdmaGetDeviceTime(uint64_t *time);

/*!
 * Check if the instrumentation is initialized and available
 * \return 1 if the instrumentation support has been sucessfully initialized,
 *         0 otherwise
 */
int xdmaInstrumentationEnabled();

/*!
 * Get the internal device address of the instrumentation timer
 * \return Address of the instrumentation times in the device,
 *         0 if any error happened
 */
uint64_t xdmaGetInstrumentationTimerAddr();

/*!
 * Initialize DMA memory subsystem
 * Any call to memory related functions such as xdmaAllocate
 * prior to memory initialization will fail.
 * \return  XDMA_SUCCESS    if memory subsystem was successfully initialized
 *          XDMA_EACCES     if user does not have permission to access memory node
 *          XDMA_ENOENT     if memory node does not exist
 *          XDMA_ERROR      in case of any other error
 */
xdma_status xdmaInitMem() {
    //Initialize dummy allocator
    pthread_mutex_init(&_allocateMutex, NULL);
    pthread_mutex_init(&_copyMutex, NULL);
    _curDevMemPtr = 0;

}

/*!
 * Deinitialize the DMA memory subsystem
 * \return  XDMA_SUCCESS    on success
 *          XDMA_ERROR      otherwise
 */
xdma_status xdmaFiniMem() {
    return XDMA_SUCCESS;    //Do nothing
}
