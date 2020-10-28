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

#include <stdio.h>
#include <stdbool.h>
#include <alloca.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include "util/queue.h"
#include <admxrc3.h>

#define MAX_DEVICES         5
//Assume there only are 1 in + 1 out channel per device
#define CHANNELS_PER_DEVICE 2
#define MAX_CHANNELS        MAX_DEVICES*CHANNELS_PER_DEVICE
#define MAX_TRANSFERS 16

#define IN_CH_STREAM 0
#define OUT_CH_STREAM 2
#define CH_MMAP 0
#define HEADER_SIZE 32

#define DMA_WRITE_TRANSACTION_LEN 16384

#define mmin(a,b) ((a)<(b)?(a):(b))

typedef unsigned char byte;

struct xdma_dev {
    byte id;
    queue_t* queue;
};

typedef enum {
    ALLOC_HOST, ALLOC_DEVICE
} alloc_type_t;

typedef enum {
    MEMCPY,
    STREAM_READ,
    STREAM_WRITE
} transfer_type_t;

// Internal library representation of an alloc
typedef struct {
    alloc_type_t type;              ///< Type of allocation
    ADMXRC3_BUFFER_HANDLE hBuffer;  ///< Userspace locked buffer handle
    union {
        void* usrPtr;               ///< Map pointer in userspace
        unsigned int devAddr;       ///< Device memory address
    };
} alloc_info_t;

// Internal library representation of a DMA transfer
typedef struct {
    transfer_type_t type;
    ADMXRC3_HANDLE hDevice, hHeader;
    ADMXRC3_TICKET ticket, headerTicket;
    ADMXRC3_BUFFER_HANDLE hHeaderBuffer;
    alloc_info_t* alloc;
    unsigned int offset;
    size_t len;
    xdma_device device;
    //NOTE: Header can also be stored in the alloc_info_t struct. All transfers are performed sequentially with the use of locks
    //NOTE: It seems that the Alphadata API only supports 256 locked buffers.
    byte header[HEADER_SIZE];
    bool used;
} transfer_info_t;

static ADMXRC3_HANDLE _hGlobalDevice;
static ADMXRC3_TICKET _streamReadHeaderTicket;
static int _validTicket;
static ADMXRC3_BUFFER_HANDLE _hGLobalHeader;
static byte _globalHeader[HEADER_SIZE];
static int _numDevices;
static struct xdma_dev _devices[MAX_DEVICES];
static int _open_cnt = 0;
static unsigned int _lastAddress;
static pthread_mutex_t _allocateMutex;
static pthread_mutex_t _transferMutex;
static pthread_mutex_t _streamReadMutex;
static pthread_mutex_t _streamWriteMutex;
static transfer_info_t _transfers[MAX_TRANSFERS];

static int getDeviceInfo(int deviceId, struct xdma_dev *devInfo);

xdma_status xdmaInitMem() {

    _lastAddress = 0;

    ADMXRC3_STATUS status = ADMXRC3_Open(0, &_hGlobalDevice);
    if (status != ADMXRC3_SUCCESS) {
        perror("Error opening global device");
        return XDMA_ERROR;
    }

    for (int i = 0; i < MAX_TRANSFERS; ++i) {
        transfer_info_t* info = &_transfers[i];
        info->alloc = NULL;
        status = ADMXRC3_Open(0, &info->hDevice);
        if (status != ADMXRC3_SUCCESS) {
            perror("Error opening device");
            for (int j = 0; j < i; ++j)
                ADMXRC3_Close(_transfers[j].hDevice);
            ADMXRC3_Close(_hGlobalDevice);
            return XDMA_ERROR;
        }
        ADMXRC3_InitializeTicket(&info->ticket);

        info->used = false;
    }

    pthread_mutex_init(&_allocateMutex, NULL);
    pthread_mutex_init(&_transferMutex, NULL);

    return XDMA_SUCCESS;
}

xdma_status xdmaOpen() {
    int open_cnt;
    ADMXRC3_STATUS status;
    xdma_status xstatus;

    // Handle multiple opens
    open_cnt = __sync_fetch_and_add(&_open_cnt, 1);
    if (open_cnt > 0) return XDMA_SUCCESS;

    xstatus = xdmaInitMem();
    if (xstatus != XDMA_SUCCESS)
        return xstatus;

    _numDevices = -1;

    pthread_mutex_init(&_streamReadMutex, NULL);
    pthread_mutex_init(&_streamWriteMutex, NULL);

    //initialize devices
    int numDevices;
    xstatus = xdmaGetNumDevices(&numDevices);
    if (xstatus != XDMA_SUCCESS) {
        return XDMA_ERROR;
    }
    xdma_device *devices;
    devices = (xdma_device*)alloca(numDevices*sizeof(xdma_device));
    xdmaGetDevices(numDevices, devices, NULL);

    for (int i = 0; i < MAX_TRANSFERS; ++i) {
        transfer_info_t* info = &_transfers[i];
        info->alloc = NULL;
        status = ADMXRC3_Open(0, &info->hHeader);
        if (status != ADMXRC3_SUCCESS) {
            perror("Error opening header device");
            return XDMA_ERROR;
        }
        ADMXRC3_InitializeTicket(&info->headerTicket);

        status = ADMXRC3_Lock(info->hHeader, &info->header[0], HEADER_SIZE, &info->hHeaderBuffer);
        if (status != ADMXRC3_SUCCESS) {
            perror("Error locking header buffer");
            return XDMA_ERROR;
        }
    }

    ADMXRC3_InitializeTicket(&_streamReadHeaderTicket);
    _validTicket = 0;
    status = ADMXRC3_Lock(_hGlobalDevice, _globalHeader, HEADER_SIZE, &_hGLobalHeader);
    if (status != ADMXRC3_SUCCESS) {
        perror("Error locking global header buffer");
        return XDMA_ERROR;
    }

    return XDMA_SUCCESS;
}

xdma_status xdmaFiniMem() {
    ADMXRC3_STATUS status;
    xdma_status ret = XDMA_SUCCESS;

    for (int i = 0; i < MAX_TRANSFERS; ++i) {
        transfer_info_t* info = &_transfers[i];

        status = ADMXRC3_Close(info->hDevice);
        if (status != ADMXRC3_SUCCESS) {
            perror("Error closing device");
            ret = XDMA_ERROR;
        }
    }

    status = ADMXRC3_Close(_hGlobalDevice);
    if (status != ADMXRC3_SUCCESS) {
        perror("Error closing device");
        ret = XDMA_ERROR;
    }

    pthread_mutex_destroy(&_allocateMutex);
    pthread_mutex_destroy(&_transferMutex);

    return ret;
}

xdma_status xdmaClose() {
    int open_cnt;
    ADMXRC3_STATUS status;
    xdma_status ret = XDMA_SUCCESS;

    // Handle multiple opens
    open_cnt = __sync_sub_and_fetch(&_open_cnt, 1);
    if (open_cnt > 0) return XDMA_SUCCESS;

    for (int i = 0; i < MAX_TRANSFERS; ++i) {
        transfer_info_t* info = &_transfers[i];

        status = ADMXRC3_Unlock(info->hHeader, info->hHeaderBuffer);
        if (status != ADMXRC3_SUCCESS) {
            perror("Error unlocking header buffer");
            ret = XDMA_ERROR;
        }

        status = ADMXRC3_Close(info->hHeader);
        if (status != ADMXRC3_SUCCESS) {
            perror("Error closing device");
            ret = XDMA_ERROR;
        }
    }

    for (int i = 0; i < _numDevices; ++i) {
        _queueFini(_devices[i].queue);
    }

    status = ADMXRC3_Unlock(_hGlobalDevice, _hGLobalHeader);
    if (status != ADMXRC3_SUCCESS) {
        perror("Error unlocking global header buffer");
        ret = XDMA_ERROR;
    }

    if (_validTicket != 0)
        ret = XDMA_ERROR;

    pthread_mutex_destroy(&_streamReadMutex);
    pthread_mutex_destroy(&_streamWriteMutex);

    if (xdmaFiniMem() != XDMA_SUCCESS)
        ret = XDMA_ERROR;

    return ret;
}

xdma_status xdmaGetNumDevices(int *numDevices) {
    char* devices = getenv("XDMA_NUM_DEVICES");
    if (devices == NULL) {
        perror("XDMA_NUM_DEVICES variable not set");
        return XDMA_ERROR;
    }
    *numDevices = atoi(devices);

    return XDMA_SUCCESS;
}

xdma_status xdmaGetDevices(int entries, xdma_device *devices, int *devs){
    xdma_status status;
    if (_numDevices < 0) { //devices have not been initialized
        int ndevs;
        status = xdmaGetNumDevices(&ndevs);
        if (status != XDMA_SUCCESS) {
            return status;
        }
        //Initialize all devices
        for (int i=0; i<ndevs; i++) {
            status = getDeviceInfo(i, &_devices[i]);
            if (status != XDMA_SUCCESS) {
                return status;
                //TODO: It may be necessary some cleanup if there is an error
            }
            _devices[i].queue = _queueInit();
            if (_devices[i].queue == NULL) {
                perror("Error initializing device queue");
                return XDMA_ERROR;
                //TODO: It may be necessary some cleanup if there is an error
            }
        }
        _numDevices = ndevs;
    }
    int retDevs;
    retDevs = (entries < _numDevices) ? entries : _numDevices;

    if (devices) {
        for (int i=0; i<retDevs; i++) {
            devices[i] = i;
        }
    }
    if (devs) {
        *devs = retDevs;
    }

    return XDMA_SUCCESS;
}

xdma_status xdmaGetDeviceChannel(xdma_device device, xdma_dir direction, xdma_channel *channel) {
    if (direction == XDMA_DEVICE_TO_DEVICE) { //Device to device
        return XDMA_ENOSYS;
    }
    *channel = device*CHANNELS_PER_DEVICE + direction;
    return XDMA_SUCCESS;
}

xdma_status xdmaAllocateHost(void **buffer, xdma_buf_handle *handle, size_t len) {
    //NOTE: DMA transfers don't send (or receive) all data unless it is multiple of 32 bytes.
    //This is because the stream bus is 256 bit wide and it doesn't have a KEEP channel
    size_t extraLength = len%32 != 0 ? (32 - len%32):0;
    void* tmp = malloc(len + extraLength);
    if (tmp == NULL) {
        return XDMA_ENOMEM;
    }
    alloc_info_t* info = (alloc_info_t*)malloc(sizeof(alloc_info_t));

    ADMXRC3_STATUS status = ADMXRC3_Lock(_hGlobalDevice, tmp, len+extraLength, &info->hBuffer);
    if (status != ADMXRC3_SUCCESS) {
        perror("Error locking buffer");
        return XDMA_ERROR;
    }
    *buffer = tmp;
    info->type = ALLOC_HOST;
    info->usrPtr = tmp;

    *handle = (xdma_buf_handle)info;

    return XDMA_SUCCESS;
}

xdma_status xdmaAllocate(xdma_buf_handle *handle, size_t len) {
    pthread_mutex_lock(&_allocateMutex);
    if (_lastAddress + len > 0x0FFFFFFFF) {
        perror("Not enough space in device memory");
        return XDMA_ENOMEM;
    }
    unsigned int address = _lastAddress;
    //NOTE: This is a dummy allocator. It should be good enough when being used by nanos.
    _lastAddress += len;
    pthread_mutex_unlock(&_allocateMutex);

    alloc_info_t* info = (alloc_info_t*)malloc(sizeof(alloc_info_t));
    info->type = ALLOC_DEVICE;
    info->devAddr = address;
    *handle = (xdma_buf_handle)info;

    return XDMA_SUCCESS;
}

xdma_status xdmaFree(xdma_buf_handle handle) {
    if (handle == NULL) {
        return XDMA_EINVAL;
    }
    alloc_info_t* info = (alloc_info_t*)handle;
    if (info->type == ALLOC_HOST) {
        ADMXRC3_STATUS status = ADMXRC3_Unlock(_hGlobalDevice, info->hBuffer);
        if (status != ADMXRC3_SUCCESS) {
            return XDMA_ERROR;
        }
        free(info->usrPtr);
    }
    free(info);
    return XDMA_SUCCESS;
}

static inline ADMXRC3_STATUS _xdmaStreamReadAsync(ADMXRC3_BUFFER_HANDLE hBuffer, void* buffer, ADMXRC3_BUFFER_HANDLE hHeaderBuffer,
                                                  ADMXRC3_HANDLE hDevice, xdma_device dev, size_t len, unsigned int offset)
{
    pthread_mutex_lock(&_streamReadMutex);
    void* data = _queueTryPop(_devices[dev].queue);
    if (data != NULL) {
        pthread_mutex_unlock(&_streamReadMutex);
        //NOTE: This implementation is supposing that the user will always request all data
        //returned by the accelerator in one call. This is good enough when used by nanos.
        memcpy(buffer, data, len);
        free(data);
        return ADMXRC3_SUCCESS;
    }

    ADMXRC3_STATUS status;
    if (_validTicket == 0) {
        status = ADMXRC3_StartReadDMALocked(_hGlobalDevice, &_streamReadHeaderTicket, OUT_CH_STREAM, 0, _hGLobalHeader, 0, HEADER_SIZE, 0);
        if (status != ADMXRC3_PENDING) {
            pthread_mutex_unlock(&_streamReadMutex);
            return status;
        }
        _validTicket = 1;
    }

    status = ADMXRC3_FinishDMA(_hGlobalDevice, &_streamReadHeaderTicket, 0);
    if (status == ADMXRC3_SUCCESS) {
        unsigned int size;
        size = 0;
        size = size | (unsigned int)(_globalHeader[1] << 8*0);
        size = size | (unsigned int)(_globalHeader[2] << 8*1);
        size = size | (unsigned int)(_globalHeader[3] << 8*2);
        size = size | (unsigned int)(_globalHeader[4] << 8*3);
        if (_globalHeader[0] != dev) {
            void* data = malloc(size);
            status = ADMXRC3_ReadDMA(hDevice, OUT_CH_STREAM, 0, data, size, 0);
            if (status != ADMXRC3_SUCCESS) {
                pthread_mutex_unlock(&_streamReadMutex);
                return status;
            }
            _queuePush(_devices[_globalHeader[0]].queue, data);
            status = ADMXRC3_StartReadDMALocked(_hGlobalDevice, &_streamReadHeaderTicket, OUT_CH_STREAM, 0, _hGLobalHeader, 0, HEADER_SIZE, 0);
        }
        else {
            size_t extraLength = len%32 != 0 ? (32 - len%32):0;
            if (len + extraLength != size) {
                perror("Requested data lenght differ from the message size returned by the accelerator");
                pthread_mutex_unlock(&_streamReadMutex);
                return ADMXRC3_UNEXPECTED_ERROR;
            }
            status = ADMXRC3_ReadDMALocked(hDevice, OUT_CH_STREAM, 0, hBuffer, offset, len, 0);
            if (status != ADMXRC3_SUCCESS) {
                pthread_mutex_unlock(&_streamReadMutex);
                return status;
            }
            if (extraLength > 0) {
                //NOTE: It seems that reading less than 32 bytes doesn't empty the queue, so although
                //the remainder will always be less than 32 bytes, we have to read that amount
                //NOTE: This call can be avoided by writing extra data in the user buffer (saving the previous data
                //and restoring it after the read)
                status = ADMXRC3_ReadDMALocked(hDevice, OUT_CH_STREAM, 0, hHeaderBuffer, 0, HEADER_SIZE, 0);
            }
            _validTicket = 0;
        }
    }

    pthread_mutex_unlock(&_streamReadMutex);
    return status;
}

static inline ADMXRC3_STATUS _xdmaStreamRead(ADMXRC3_BUFFER_HANDLE hBuffer, void* buffer, ADMXRC3_BUFFER_HANDLE hHeaderBuffer,
                                             ADMXRC3_HANDLE hDevice, xdma_device dev, size_t len, unsigned int offset)
{
    ADMXRC3_STATUS status;
    do {
        status = _xdmaStreamReadAsync(hBuffer, buffer, hHeaderBuffer, hDevice, dev, len, offset);
    }
    while(status == ADMXRC3_PENDING);
    return status;
}

static inline xdma_status _xdmaStream(xdma_buf_handle buffer, size_t len, unsigned int offset,
        xdma_device ignored, xdma_channel channel, bool block, xdma_transfer_handle *transfer)
{
    ADMXRC3_TICKET* ticket, *headerTicket;
    ADMXRC3_HANDLE hDevice, hHeader;
    ADMXRC3_BUFFER_HANDLE hBuffer, hHeaderBuffer;
    ADMXRC3_STATUS status;
    byte* header;
    alloc_info_t* info = (alloc_info_t*) buffer;
    xdma_device dev = channel/CHANNELS_PER_DEVICE;
    unsigned int ch = channel%CHANNELS_PER_DEVICE;

    if (info->type != ALLOC_HOST) {
        return XDMA_EINVAL;
    }

    unsigned int currentTransfer = 0;
    unsigned int i;
    pthread_mutex_lock(&_transferMutex);
    for (i = 0; i < MAX_TRANSFERS; ++i) {
        if (!_transfers[i].used) {
            currentTransfer = i;
            _transfers[i].used = true;
            break;
        }
    }
    pthread_mutex_unlock(&_transferMutex);
    if (i >= MAX_TRANSFERS) {
        printf("No more transfers available in device");
        return XDMA_ERROR;
    }

    if (!block) {
        ticket = &_transfers[currentTransfer].ticket;
        headerTicket = &_transfers[currentTransfer].headerTicket;
    }
    hDevice = _transfers[currentTransfer].hDevice;
    hHeader = _transfers[currentTransfer].hHeader;
    header  = &_transfers[currentTransfer].header[0];
    hHeaderBuffer = _transfers[currentTransfer].hHeaderBuffer;
    hBuffer = info->hBuffer;

    size_t extraLength = len%32 != 0 ? (32 - len%32):0;

    if (ch == XDMA_TO_DEVICE) {
        header[0] = (byte)dev;
        header[1] = (len >> 8*0) & 0xFF;
        header[2] = (len >> 8*1) & 0xFF;
        header[3] = (len >> 8*2) & 0xFF;
        header[4] = (len >> 8*3) & 0xFF;
        pthread_mutex_lock(&_streamWriteMutex);
        if (block) {
            //NOTE: There are a lot of cycles in the FPGA (more than 512) in which
            //it remains idle, waiting for the data between both calls to writeDMA
            status = ADMXRC3_WriteDMALocked(hDevice, IN_CH_STREAM, 0, hHeaderBuffer, 0, HEADER_SIZE, 0);
            if (status != ADMXRC3_SUCCESS) {
                perror("Error in write operation of stream channel");
                pthread_mutex_unlock(&_streamWriteMutex);
                return XDMA_ERROR;
            }
            status = ADMXRC3_WriteDMALocked(hDevice, IN_CH_STREAM, 0, hBuffer, offset, len + extraLength, 0);
            if (status != ADMXRC3_SUCCESS) {
                perror("Error in write operation of stream channel");
                pthread_mutex_unlock(&_streamWriteMutex);
                return XDMA_ERROR;
            }
            _transfers[currentTransfer].used = false;
        }
        else {
            status = ADMXRC3_StartWriteDMALocked(hHeader, headerTicket, IN_CH_STREAM, 0, hHeaderBuffer, 0, HEADER_SIZE, 0);
            if (status != ADMXRC3_PENDING) {
                perror("Error submitting transfer to stream channel");
                pthread_mutex_unlock(&_streamWriteMutex);
                return XDMA_ERROR;
            }
            status = ADMXRC3_StartWriteDMALocked(hDevice, ticket, IN_CH_STREAM, 0, hBuffer, offset, len + extraLength, 0);
            if (status != ADMXRC3_PENDING) {
                perror("Error submitting transfer to stream channel");
                pthread_mutex_unlock(&_streamWriteMutex);
                return XDMA_ERROR;
            }
            *transfer = (xdma_transfer_handle)currentTransfer;
            _transfers[currentTransfer].type = STREAM_WRITE;
        }
        pthread_mutex_unlock(&_streamWriteMutex);
    }
    else if (ch == XDMA_FROM_DEVICE) {
        if (block) {
            status = _xdmaStreamRead(hBuffer, info->usrPtr, hHeaderBuffer, hDevice, dev, len, offset);
            if (status != ADMXRC3_SUCCESS) {
                perror("Error in read operation of stream channel");
                return XDMA_ERROR;
            }
            _transfers[currentTransfer].used = false;
        }
        else {
            *transfer = (xdma_transfer_handle)currentTransfer;
            pthread_mutex_lock(&_streamReadMutex);
            if (_validTicket == 0) {
                status = ADMXRC3_StartReadDMALocked(_hGlobalDevice, &_streamReadHeaderTicket, OUT_CH_STREAM, 0, _hGLobalHeader, 0, HEADER_SIZE, 0);
                if (status != ADMXRC3_PENDING) {
                    perror("Error submitting transfer to stream channel");
                    pthread_mutex_unlock(&_streamReadMutex);
                    return XDMA_ERROR;
                }
                _validTicket = 1;
            }
            pthread_mutex_unlock(&_streamReadMutex);
            _transfers[currentTransfer].type = STREAM_READ;
            _transfers[currentTransfer].alloc = (alloc_info_t*) buffer;
            _transfers[currentTransfer].offset = offset;
            _transfers[currentTransfer].len = len;
            _transfers[currentTransfer].device = dev;
        }
    }

    return XDMA_SUCCESS;
}

xdma_status xdmaStream(xdma_buf_handle buffer, size_t len, unsigned int offset,
                       xdma_device dev, xdma_channel channel)
{
    return _xdmaStream(buffer, len, offset, dev, channel, 1 /*block*/, NULL);
}

xdma_status xdmaStreamAsync(xdma_buf_handle buffer, size_t len, unsigned int offset,
                            xdma_device dev, xdma_channel channel, xdma_transfer_handle *transfer) {
    return _xdmaStream(buffer, len, offset, dev, channel, 0 /*block*/, transfer);
}

static inline xdma_status _xdmaMemcpy(void *usr, xdma_buf_handle buffer, size_t len, unsigned int offset,
                xdma_dir mode, bool block, xdma_transfer_handle *transfer)
{
    ADMXRC3_TICKET* ticket;
    ADMXRC3_HANDLE hDevice;
    ADMXRC3_STATUS status = ADMXRC3_SUCCESS;
    alloc_info_t* info = (alloc_info_t*) buffer;

    //Copy from userspace memory to locked buffer
    if (info->type == ALLOC_HOST) {
        alloc_info_t* info = (alloc_info_t*)buffer;
        byte* lockedBuffer = info->usrPtr;
        memcpy(&lockedBuffer[offset], usr, len);
        return XDMA_SUCCESS;
    }

    unsigned int currentTransfer;
    if (!block) {
        unsigned int i;
        pthread_mutex_lock(&_transferMutex);
        for (i = 0; i < MAX_TRANSFERS; ++i) {
            if (!_transfers[i].used) {
                currentTransfer = i;
                _transfers[i].used = true;
                break;
            }
        }
        pthread_mutex_unlock(&_transferMutex);
        if (i >= MAX_TRANSFERS) {
            perror("No more transfers available in device");
            return XDMA_ERROR;
        }
        ticket = &_transfers[currentTransfer].ticket;
        hDevice = _transfers[currentTransfer].hDevice;
    }

    unsigned int base = info->devAddr;
    unsigned int bufferAddress = base+offset;

    if (mode == XDMA_TO_DEVICE) {
        if (block) {
            pthread_mutex_lock(&_transferMutex);
            ADMXRC3_BUFFER_HANDLE hBuffer;
            status = ADMXRC3_Lock(_hGlobalDevice, usr, len, &hBuffer);
            if (status != ADMXRC3_SUCCESS) {
                perror("Error locking buffer");
                return XDMA_ERROR;
            }
            //printf("Doing write of len %lu on address 0x%X\n", len, bufferAddress);
            for (size_t i = 0; i < len; i += DMA_WRITE_TRANSACTION_LEN) {
                size_t actualLen = mmin(len-i, DMA_WRITE_TRANSACTION_LEN);
                status = ADMXRC3_WriteDMALocked(_hGlobalDevice, CH_MMAP, 0, hBuffer, i, actualLen, bufferAddress + i);
                if (status != ADMXRC3_SUCCESS) {
                    perror("Error in write operation of mmap channel");
                    ADMXRC3_Unlock(_hGlobalDevice, hBuffer);
                    return XDMA_ERROR;
                }
            }
            ADMXRC3_Unlock(_hGlobalDevice, hBuffer);
            pthread_mutex_unlock(&_transferMutex);
            //status = ADMXRC3_WriteDMA(_hGlobalDevice, CH_MMAP, 0, usr, len, bufferAddress);
            //printf("After write\n");
        }
        else {
            status = ADMXRC3_StartWriteDMA(hDevice, ticket, CH_MMAP, 0, usr, len, bufferAddress);
        }
    } else if (mode == XDMA_FROM_DEVICE) {
        if (block) {
            status = ADMXRC3_ReadDMA(_hGlobalDevice, CH_MMAP, 0, usr, len, bufferAddress);
        }
        else {
            status = ADMXRC3_StartReadDMA(hDevice, ticket, CH_MMAP, 0, usr, len, bufferAddress);
        }
    } else if (mode == XDMA_DEVICE_TO_DEVICE) {
        return XDMA_ENOSYS;
    } else {
        return XDMA_EINVAL;
    }

    if (!block) {
        if (status != ADMXRC3_PENDING) {
            perror("Error submitting transfer to mmap channel");
            return XDMA_ERROR;
        }
        *transfer = (xdma_transfer_handle)currentTransfer;
        _transfers[currentTransfer].type = MEMCPY;
    }
    else if (status != ADMXRC3_SUCCESS) {
        char* s = (char*)malloc(256);
        printf("Len is %d and buffer address is %d\n", (int)len, (int)bufferAddress);
        sprintf(s, "Error in %s operation of mmap channel. ADMXRC3_STATUS is %s: %s. errno", mode == XDMA_TO_DEVICE ? "write":"read",
                ADMXRC3_GetStatusStringA(status, true), ADMXRC3_GetStatusStringA(status, false));
        perror(s);
        free(s);
        return XDMA_ERROR;
    }

    return XDMA_SUCCESS;
}

xdma_status xdmaMemcpy(void *usr, xdma_buf_handle buffer, size_t len, unsigned int offset,
        xdma_dir mode)

{
    return _xdmaMemcpy(usr, buffer, len, offset, mode, 1 /*block*/, NULL /*xdma_transfer_handle*/);
}

xdma_status xdmaMemcpyAsync(void *usr, xdma_buf_handle buffer, size_t len, unsigned int offset,
        xdma_dir mode, xdma_transfer_handle *transfer)

{
    return _xdmaMemcpy(usr, buffer, len, offset, mode, 0 /*block*/, transfer);
}

static inline xdma_status _xdmaFinishTransfer(xdma_transfer_handle transfer, bool block) {
    ADMXRC3_TICKET* ticket = &_transfers[transfer].ticket;
    ADMXRC3_HANDLE hDevice = _transfers[transfer].hDevice;
    ADMXRC3_STATUS status;

    if (_transfers[transfer].type == STREAM_READ) {
        ADMXRC3_BUFFER_HANDLE hBuffer = _transfers[transfer].alloc->hBuffer;
        ADMXRC3_BUFFER_HANDLE hHeaderBuffer = _transfers[transfer].hHeaderBuffer;
        void* usrPtr = _transfers[transfer].alloc->usrPtr;
        unsigned int offset = _transfers[transfer].offset;
        size_t len = _transfers[transfer].len;
        xdma_device dev = _transfers[transfer].device;

        if (block)
            status = _xdmaStreamRead(hBuffer, usrPtr, hHeaderBuffer, hDevice, dev, len, offset);
        else
            status = _xdmaStreamReadAsync(hBuffer, usrPtr, hHeaderBuffer, hDevice, dev, len, offset);

        if (status == ADMXRC3_PENDING) {
            return XDMA_PENDING;
        }
        else if (status != ADMXRC3_SUCCESS)
            return XDMA_ERROR;

        _transfers[transfer].used = false;
        return XDMA_SUCCESS;
    }
    else if (_transfers[transfer].type == STREAM_WRITE) {
        ADMXRC3_HANDLE hHeader = _transfers[transfer].hHeader;
        ADMXRC3_TICKET* headerTicket = &_transfers[transfer].headerTicket;
        //On stream writes, we have to check that the header is correctly submitted first
        status = ADMXRC3_FinishDMA(hHeader, headerTicket, block);
        if (status == ADMXRC3_PENDING) {
            return XDMA_PENDING;
        }
        else if (status != ADMXRC3_SUCCESS) {
            return XDMA_ERROR;
        }
    }

    status = ADMXRC3_FinishDMA(hDevice, ticket, block);
    if (status == ADMXRC3_PENDING) {
        return XDMA_PENDING;
    }
    else if (status != ADMXRC3_SUCCESS) {
        return XDMA_ERROR;
    }

    _transfers[transfer].used = false;

    return XDMA_SUCCESS;
}

xdma_status xdmaTestTransfer(xdma_transfer_handle *transfer){
    xdma_status ret = _xdmaFinishTransfer(*transfer, 0 /*block*/);
    return ret;
}

xdma_status xdmaWaitTransfer(xdma_transfer_handle *transfer){
    xdma_status ret = _xdmaFinishTransfer(*transfer, 1 /*block*/);
    return ret;
}

static int getDeviceInfo(int deviceId, struct xdma_dev *devInfo) {
    devInfo->id = (byte)deviceId;
    return XDMA_SUCCESS;
}


xdma_status xdmaGetDeviceAddress(xdma_buf_handle buffer, unsigned long *address) {
    alloc_info_t* info = (alloc_info_t*) buffer;

    if (info->type == ALLOC_HOST) {
        return XDMA_ERROR;
    }
    else {
        *address = info->devAddr;
    }

    return XDMA_SUCCESS;
}

xdma_status xdmaInitHWInstrumentation() {

    return XDMA_ENOSYS;
}

xdma_status xdmaFiniHWInstrumentation() {
    return XDMA_ENOSYS;
}

xdma_status xdmaGetDeviceTime(uint64_t *time) {
    return XDMA_ENOSYS;
}

int xdmaInstrumentationEnabled() {
    return -1;
}

uint64_t xdmaGetInstrumentationTimerAddr() {
    return 0;
}
