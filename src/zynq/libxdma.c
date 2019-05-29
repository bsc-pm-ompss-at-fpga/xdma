#include "../libxdma.h"

#define XDMA_FILEPATH   "/dev/ompss_fpga/xdma"
#define INSTR_FILEPATH  "/dev/ompss_fpga/hw_instrumentation"
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
static int _mem_fd = 0;

static int _numDevices;
static struct xdma_dev _devices[MAX_DEVICES];
static struct xdma_chan_cfg _channels[MAX_CHANNELS];
static unsigned int _kUsedSpace;
static pthread_mutex_t _allocateMutex;
static pthread_mutex_t *_submitMutexes;
static int _open_cnt = 0;

static int getDeviceInfo(int deviceId, struct xdma_dev *devInfo);
static xdma_status xdmaOpenChannel(xdma_device device, xdma_dir direction);

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

xdma_status xdmaOpen() {
    int open_cnt;

    // Handle multiple opens
    open_cnt = __sync_fetch_and_add(&_open_cnt, 1);
    if (open_cnt > 0) return XDMA_SUCCESS;

    _numDevices = -1;
    _kUsedSpace = 0;
    _instr_fd = 0;
    //TODO: check if library has been initialized
    _fd = open(XDMA_FILEPATH, O_RDWR | O_TRUNC);
    if (_fd == -1) {
        //perror("Error opening file for writing");
        xdma_status ret = XDMA_ERROR;
        switch (errno) {
            case EACCES: //User cannot access device
                ret = XDMA_EACCES;
                break;
            case ENOENT: //Device not available
                ret = XDMA_ENOENT;
                break;
        }
        return ret;
    }


    //try to initialize instrumentation support
    // NOTE: Initialization disabled as instrumentation may be not-wanted
    //xdmaInitHWInstrumentation();

    //initialize devices
    int numDevices;
    xdmaGetNumDevices(&numDevices);
    xdma_device *devices;
    devices = (xdma_device*)alloca(numDevices*sizeof(xdma_device));
    xdmaGetDevices(numDevices, devices, NULL);
    for (int i=0; i<numDevices; i++) {
        //need te run channel configuration to initialize the channel table
        xdmaOpenChannel(devices[i], XDMA_FROM_DEVICE);
        xdmaOpenChannel(devices[i], XDMA_TO_DEVICE);
    }

    return XDMA_SUCCESS;
}

xdma_status xdmaClose() {
    int open_cnt;

    // Handle multiple opens
    open_cnt = __sync_sub_and_fetch(&_open_cnt, 1);
    if (open_cnt > 0) return XDMA_SUCCESS;

    //Device mutex finalization
    if (_numDevices >= 0) {
        //If anyone called xdmaGetDevices, the global variable is negative and mutexes are not init
        for (int i=0; i<_numDevices; i++) {
            pthread_mutex_destroy(&(_submitMutexes[i]));
        }
        free(_submitMutexes);
    }

    if (close(_fd) == -1) {
        perror("Error closing device file");
        return XDMA_ERROR;
    }

    return XDMA_SUCCESS;
}

xdma_status xdmaGetNumDevices(int *numDevices){

    if (ioctl(_fd, XDMA_GET_NUM_DEVICES, numDevices) < 0) {
        perror("Error ioctl getting device num");
        return XDMA_ERROR;
    }
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
        _submitMutexes = (pthread_mutex_t *)(malloc(ndevs*sizeof(pthread_mutex_t)));
        if (_submitMutexes == NULL) {
            return XDMA_ENOMEM;
        }
        //Initialize all devices
        for (int i=0; i<ndevs; i++) {
            status = getDeviceInfo(i, &_devices[i]);
            if (status != XDMA_SUCCESS) {
                return status;
                //TODO: It may be necessary some cleanup if there is an error
            }

            //Initialize device mutex
            pthread_mutex_init(&(_submitMutexes[i]), NULL);
        }
        _numDevices = ndevs;
    }
    int retDevs;
    retDevs = (entries < _numDevices) ? entries : _numDevices;

    if (devices) {
        for (int i=0; i<retDevs; i++) {
            devices[i] = (xdma_device) &_devices[i];
        }
    }
    if (devs) {
        *devs = retDevs;
    }

    return XDMA_SUCCESS;
}

xdma_status xdmaGetDeviceChannel(xdma_device device, xdma_dir direction, xdma_channel *channel) {
    struct xdma_chan_cfg *ch_config;
    struct xdma_dev *dev;

    dev = (struct xdma_dev*)device;
    int devNumber = (dev - _devices);
    ch_config = &_channels[devNumber*CHANNELS_PER_DEVICE + direction];
    *channel = (xdma_channel)ch_config;

    return XDMA_SUCCESS;
}

static xdma_status xdmaOpenChannel(xdma_device device, xdma_dir direction) {
    struct xdma_chan_cfg *ch_config;
    struct xdma_dev *dev;

    dev = (struct xdma_dev*)device;
    int devNumber = (dev - _devices);
    //direction is going to be 0 or 1
    ch_config = &_channels[devNumber*CHANNELS_PER_DEVICE + direction];

    if (direction == XDMA_FROM_DEVICE) {
        ch_config->chan = dev->rx_chan;
        ch_config->dir = XDMA_DEV_TO_MEM;
    } else { //Should be either FROM device or TO device
        ch_config->chan = dev->tx_chan;
        ch_config->dir = XDMA_MEM_TO_DEV;
    }

    ch_config->coalesc = XDMA_CH_CFG_COALESC_DEF;
    ch_config->delay = XDMA_CH_CFG_DELAY_DEF;
    ch_config->reset = XDMA_CH_CFG_RESET_DEF;
    if (ioctl(_fd, XDMA_DEVICE_CONTROL, ch_config) < 0) {
        perror("Error ioctl config rx chan");
        return XDMA_ERROR;
    }
    return XDMA_SUCCESS;
}

xdma_status _getDeviceAddress(unsigned long dmaBuffer, unsigned long *address) {
    union {
        unsigned long dmaBuffer;
        unsigned long dmaAddress;
    } kArg;
    int status;
    kArg.dmaBuffer = dmaBuffer;
    status = ioctl(_mem_fd, XDMAMEM_GET_DMA_ADDRESS, &kArg);
    if (status) {
        perror("Error getting DMA address");
        return XDMA_ERROR;
    } else {
        *address = kArg.dmaAddress;
        return XDMA_SUCCESS;
    }
}

xdma_status xdmaAllocateHost(void **buffer, xdma_buf_handle *handle, size_t len) {
    //TODO: Check that mmap + ioctl are performet atomically
    unsigned long ret, devAddress;
    unsigned int status;
    pthread_mutex_lock(&_allocateMutex);
    *buffer = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, _mem_fd, 0);
    if (*buffer == MAP_FAILED) {
        pthread_mutex_unlock(&_allocateMutex);
        return XDMA_ENOMEM;
    }
    //get the handle for the allocated buffer
    status = ioctl(_mem_fd, XDMAMEM_GET_LAST_KBUF, &ret);
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
    return XDMA_SUCCESS;
}

xdma_status xdmaAllocate(xdma_buf_handle *handle, size_t len) {
    void *ptr;
    alloc_info_t *info;
    xdma_status stat = xdmaAllocateHost(&ptr, (xdma_buf_handle)&info, len);
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
    size = ioctl(_mem_fd, XDMAMEM_RELEASE_KBUF, &info->handle);
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

static inline xdma_status _xdmaStream(xdma_buf_handle buffer, size_t len, unsigned int offset,
        xdma_device dev, xdma_channel ch, bool block, xdma_transfer_handle *transfer)
{
    struct xdma_chan_cfg *channel = (struct xdma_chan_cfg*)ch;
    struct xdma_dev *device = (struct xdma_dev*)dev;
    alloc_info_t *bInfo = (alloc_info_t*)buffer;
    //prepare kernel mapped buffer & submit
    struct xdma_buf_info buf;
    buf.chan = channel->chan;
    //Weird. Completion callback may be stored per channel
    buf.completion =
        (channel->dir == XDMA_DEV_TO_MEM) ? device->rx_cmp : device->tx_cmp;
    buf.cookie = (dma_cookie_t)0;
    //Use address to store the buffer descriptor handle
    buf.address = (unsigned long)bInfo->devAddr;
    buf.buf_offset = offset;
    buf.buf_size = len;
    buf.dir = channel->dir;

    pthread_mutex_t * devMutex = &(_submitMutexes[device - _devices]);
    pthread_mutex_lock(devMutex);
    if (ioctl(_fd, XDMA_PREP_BUF, &buf) < 0) {
        pthread_mutex_unlock(devMutex);
        perror("Error ioctl set rx buf");
        return XDMA_ERROR;
    }

    //start input transfer host->dev
    struct xdma_transfer *trans;
    //May not be a good thing for the performance to allocate things for each transfer
    if (block) {
        //If we are waiting for the transfer to finish, we can allocate
        //the data structure in the stack
        trans = (struct xdma_transfer*)alloca(sizeof(struct xdma_transfer));
    } else {
        trans = (struct xdma_transfer*)malloc(sizeof(struct xdma_transfer));
    }
    trans->chan = channel->chan;
    trans->completion = buf.completion;
    trans->cookie = buf.cookie;
    trans->wait = block;
    trans->sg_transfer = 0;
    if (ioctl(_fd, XDMA_START_TRANSFER, trans) < 0) {
        pthread_mutex_unlock(devMutex);
        perror("Error ioctl start tx trans");
        return XDMA_ERROR;
    }
    pthread_mutex_unlock(devMutex);
    if (!block) {
        *transfer = (xdma_transfer_handle)trans;
    }
    return XDMA_SUCCESS;
}

xdma_status xdmaStream(xdma_buf_handle buffer, size_t len, unsigned int offset,
            xdma_device dev, xdma_channel channel)
{
    return _xdmaStream(buffer, len, offset, dev, channel, 1 /*block*/, NULL /*xdma_transfer_handle*/);
}

xdma_status xdmaStreamAsync(xdma_buf_handle buffer, size_t len, unsigned int offset,
        xdma_device dev, xdma_channel channel, xdma_transfer_handle *transfer)
{
    return _xdmaStream(buffer, len, offset, dev, channel, 0 /*block*/, transfer);
}

xdma_status xdmaMemcpy(void *usr, xdma_buf_handle buffer, size_t len, unsigned int offset,
        xdma_dir mode)
{
    alloc_info_t *info = (alloc_info_t *)buffer;
    xdma_status ret = XDMA_SUCCESS;
    if (mode == XDMA_TO_DEVICE) {
        //memcpy(((unsigned char *)info->ptr) + offset, usr, len);
        int i;
        uint64_t *lsrc, *ldst;
        lsrc = (uint64_t*)usr;
        ldst = (uint64_t*)((unsigned char*)info->ptr + offset);
        //do not allow unaligned transfers
        if ((uintptr_t)ldst % sizeof(uint64_t)) {
            return XDMA_ERROR;
        }

        for (i=0; i<len/sizeof(uint64_t); i++) {
            ldst[i] = lsrc[i];
        }
        i *= sizeof(uint64_t);
        char *src, *dst;
        src = usr;
        dst = info->ptr + offset;
        for (; i<len; i++) {
            dst[i] = src[i];
        }

    } else if (mode == XDMA_FROM_DEVICE) {
        memcpy(usr, ((unsigned char *)info->ptr) + offset, len);
    } else if (mode == XDMA_DEVICE_TO_DEVICE) {
        ret = XDMA_ENOSYS;
    } else {
        ret = XDMA_EINVAL;
    }
    return ret;
}

xdma_status xdmaMemcpyAsync(void *usr, xdma_buf_handle buffer, size_t len, unsigned int offset,
        xdma_dir mode, xdma_transfer_handle *transfer)
{
    *transfer = (xdma_transfer_handle)NULL;
    return xdmaMemcpy(usr, buffer, len, offset, mode);
}

static inline xdma_status _xdmaFinishTransfer(xdma_transfer_handle transfer, bool block) {
    struct xdma_transfer *trans = (struct xdma_transfer *)transfer;
    int status;

    if (trans == NULL) {
        //Memcpy or uninitialized/cleaned up transfer
        return XDMA_SUCCESS;
    }

    trans->wait = block;
    status = ioctl(_fd, XDMA_FINISH_TRANSFER, trans);
    if (status < 0) {
        perror("Transfer finish error\n");
        return XDMA_ERROR;
    } else if (status == XDMA_DMA_TRANSFER_PENDING) {
        return XDMA_PENDING;
    } else {    //XDMA_SUCCESS
        return XDMA_SUCCESS;
    }
}

static inline xdma_status _xdmaReleaseTransfer(xdma_transfer_handle *transfer){
    struct xdma_transfer *trans = (struct xdma_transfer *)*transfer;
    int status;

    if (trans == NULL) {
        //Memcpy or uninitialized/cleaned up transfer
        return XDMA_SUCCESS;
    }

    if (trans->sg_transfer) {
        //transfer is a SG one, some kernel resources should be released
        status = ioctl(_fd, XDMA_RELEASE_USR_BUF, (unsigned long)trans->sg_transfer);
        if (status) {
            perror("Error releasing kernel SG resources");
            printf("  from transfer %lx\n", *transfer);
        }
    }

    free(trans);
    *transfer = (xdma_transfer_handle)NULL;

    //TODO: reuse allocated transfer structures
    return XDMA_SUCCESS;
}

xdma_status xdmaTestTransfer(xdma_transfer_handle *transfer){
    xdma_status ret = _xdmaFinishTransfer(*transfer, 0 /*block*/);
    if (ret == XDMA_SUCCESS || ret == XDMA_ERROR) {
        _xdmaReleaseTransfer(transfer);
    }
    return ret;
}

xdma_status xdmaWaitTransfer(xdma_transfer_handle *transfer){
    xdma_status ret = _xdmaFinishTransfer(*transfer, 1 /*block*/);
    if (ret == XDMA_SUCCESS || ret == XDMA_PENDING || ret == XDMA_ERROR) {
        if (ret == XDMA_PENDING) {
            fprintf(stderr, "Warining: transfer %x timed out\n", (unsigned int)*transfer);
        }
        _xdmaReleaseTransfer(transfer);
    }
    return ret;
}

static int getDeviceInfo(int deviceId, struct xdma_dev *devInfo) {
    devInfo->tx_chan = NULL;
    devInfo->tx_cmp = NULL;
    devInfo->rx_chan = NULL;
    devInfo->rx_cmp = NULL;
    devInfo->device_id = deviceId;
    if (ioctl(_fd, XDMA_GET_DEV_INFO, devInfo) < 0) {
        perror("Error ioctl getting device info");
        return XDMA_ERROR;
    }
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

xdma_status xdmaInitMem() {
    if (_mem_fd > 0) return XDMA_SUCCESS;
    _mem_fd = open(MEM_FILEPATH, O_RDWR);
    if (_mem_fd == -1) {
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

xdma_status xdmaFiniMem() {
    if (_mem_fd > 0) {
        close(_instr_fd);
    }
    //Mutex finalization
    pthread_mutex_destroy(&_allocateMutex);

    return XDMA_SUCCESS;
}

xdma_status xdmaGetDeviceTime(uint64_t *time) {
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

uint64_t xdmaGetInstrumentationTimerAddr() {
    if (_instr_fd <= 0) return 0;

    int status;
    uint64_t addr;
    status = ioctl(_instr_fd, HWINSTR_GET_ADDR, &addr);
    if (status) {
        perror("Could not get instrumentation address");
        return 0;
    }
    return addr;
}

#undef XDMA_FILEPATH
#undef INSTR_FILEPATH
#undef MAP_SIZE
#undef FILESIZE
