#include "../libxdma.h"

#define XDMA_FILEPATH   "/dev/ompss_fpga/xdma"
#define INSTR_FILEPATH  "/dev/ompss_fpga/hwcounter"
#define MEM_FILEPATH    "/dev/ompss_fpga/xdma_mem"
#define DEV_MEM_FILEPATH    "/dev/ompss_fpga/xdma_dev_mem"
#define MAP_SIZE  (33554432)
#define FILESIZE (MAP_SIZE * sizeof(uint8_t))

#define DEV_MEM_SIZE        0x400000000 ///<Device memory (16GB)
#define DEV_BASE_ADDR       0x2000000000
#define DEV_ALIGN           (512/8)

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
#define MAX_DMA_TRANSFER_SIZE   (4096*254)  //254 pages

static int _fd;
static int _instr_fd = 0;
static int _mem_fd = 0;
static int _dev_mem_fd = 0;

static uintptr_t _devMem;
static uintptr_t _curDevMemPtr;

static int _numDevices;
static struct xdma_dev _devices[MAX_DEVICES];
static struct xdma_chan_cfg _channels[MAX_CHANNELS];
static unsigned int _kUsedSpace;
static pthread_mutex_t _allocateMutex;
static pthread_mutex_t *_submitMutexes;
static pthread_mutex_t _copyMutex;
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

//xdma_status xdmaAllocateHost(void **buffer, xdma_buf_handle *handle, size_t len) {
//    //TODO: Check that mmap + ioctl are performet atomically
//    unsigned long ret, devAddress;
//    unsigned int status;
//    pthread_mutex_lock(&_allocateMutex);
//    *buffer = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, _mem_fd, 0);
//    if (*buffer == MAP_FAILED) {
//        pthread_mutex_unlock(&_allocateMutex);
//        return XDMA_ENOMEM;
//    }
//    //get the handle for the allocated buffer
//    status = ioctl(_mem_fd, XDMAMEM_GET_LAST_KBUF, &ret);
//    pthread_mutex_unlock(&_allocateMutex);
//    if (status) {
//        perror("Error allocating pinned memory");
//        munmap(buffer, len);
//        return XDMA_ERROR;
//    }
//
//    status = _getDeviceAddress(ret, &devAddress);
//
//    alloc_info_t *info = (alloc_info_t *)malloc(sizeof(alloc_info_t));
//    info->type = ALLOC_HOST;
//    info->handle = ret;
//    info->ptr = *buffer;
//    info->devAddr = devAddress;
//    *handle = (xdma_buf_handle)info;
//    return XDMA_SUCCESS;
//}

xdma_status xdmaAllocateHost(void **buffer, xdma_buf_handle *handle, size_t len) {

    xdma_status status;
    status = xdmaAllocate(handle, len);
    alloc_info_t *info = (alloc_info_t *)*handle;
    *buffer = info->ptr;
    return status;
}
xdma_status xdmaAllocate(xdma_buf_handle *handle, size_t len) {
    void *ptr;
    size_t nlen;

    pthread_mutex_lock(&_allocateMutex);
    nlen = ((len + (DEV_ALIGN + 1))/DEV_ALIGN)*DEV_ALIGN;
    //adjust size so we always get aligned addresses
    if (_curDevMemPtr + nlen > _devMem + DEV_MEM_SIZE) {
        pthread_mutex_unlock(&_allocateMutex);
        return XDMA_ENOMEM;
    }
    ptr = (void*)_curDevMemPtr;
    _curDevMemPtr += nlen;
    pthread_mutex_unlock(&_allocateMutex);

    alloc_info_t *info = (alloc_info_t *)malloc(sizeof(alloc_info_t));
    info->type = ALLOC_DEVICE;
    info->ptr = ptr;
    info->devAddr = ((uintptr_t)ptr - _devMem) + DEV_BASE_ADDR;

    *handle = (xdma_buf_handle)info;
    return XDMA_SUCCESS;
}

xdma_status xdmaFreeHostMem(alloc_info_t * info) {
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
    return ret;
}

xdma_status xdmaFreeDevMem(alloc_info_t *info) {
    //Do nothing as we cannot release a random allocated region
    return XDMA_SUCCESS;
}

xdma_status xdmaFree(xdma_buf_handle handle) {
    if (handle == NULL) {
        return XDMA_EINVAL;
    }
    alloc_info_t *info = (alloc_info_t *)handle;
    xdma_status ret;
    if (info->type == ALLOC_DEVICE) ret = xdmaFreeDevMem(info);
    else ret = xdmaFreeHostMem(info);
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

#define USE_CDMA
xdma_status xdmaMemcpy(void *usr, xdma_buf_handle buffer, size_t len, unsigned int offset,
        xdma_dir mode)
{
#ifdef USE_CDMA
    xdma_transfer_handle handle;
    xdmaMemcpyAsync(usr, buffer, len, offset, mode, &handle);
    xdmaWaitTransfer(&handle);
    //TODO error management
    return XDMA_SUCCESS;
#else
    alloc_info_t *info = (alloc_info_t*)buffer;
    if (mode == XDMA_TO_DEVICE) {
        memcpy(((unsigned char *)info->ptr) + offset, usr, len);
    } else {
        memcpy(usr, ((unsigned char *)info->ptr) + offset, len);
    }
    return XDMA_SUCCESS;
#endif
}


xdma_status xdmaMemcpyAsyncChunk(void *usr, xdma_buf_handle buffer, size_t len,
        unsigned int offset, xdma_dir mode, xdma_transfer_handle *transfer)
{
//    *transfer = (xdma_transfer_handle)NULL;
//    return xdmaMemcpy(usr, buffer, len, offset, mode);


    int status;
	struct xdma_memcpy_info memcpyInfo;
    alloc_info_t *bInfo = (alloc_info_t*)buffer;

    if (mode == XDMA_TO_DEVICE) {
        memcpyInfo.src_address = (unsigned long)usr;
        memcpyInfo.src_offset = 0;
        memcpyInfo.dst_address = bInfo->devAddr;
        memcpyInfo.dst_offset = offset;
        memcpyInfo.direction = XDMA_MEM_TO_DEV;

    } else if (mode == XDMA_FROM_DEVICE) {
        memcpyInfo.src_address = bInfo->devAddr;
        memcpyInfo.src_offset = offset;
        memcpyInfo.dst_address = (unsigned long)usr;
        memcpyInfo.dst_offset = 0;
        memcpyInfo.direction = XDMA_DEV_TO_MEM;
    } else {
        //dev to dev is currently unsupported
    }
	memcpyInfo.size = len;

	status = ioctl(_fd, XDMA_PREP_MEMCPY, &memcpyInfo);
	if (status < 0) {
		perror("cdma_memcpy failed\n");
		return XDMA_ERROR;
	}

	struct xdma_transfer *trans = malloc(sizeof(struct xdma_transfer));

	trans->chan = memcpyInfo.chan;
	trans->completion = memcpyInfo.completion;
	trans->cookie = memcpyInfo.cookie;
	trans->wait = 0;    //do not wait
	trans->sg_transfer = memcpyInfo.sg_transfer;
	status = ioctl(_fd, XDMA_START_TRANSFER, trans);
	if (status < 0) {
		perror("Error submitting cdma transfer");
		return XDMA_ERROR;
	}
	*transfer = (xdma_transfer_handle)trans;
	return XDMA_SUCCESS;

}

xdma_status xdmaMemcpyAsync(void *usr, xdma_buf_handle buffer, size_t len,
        unsigned int offset, xdma_dir mode, xdma_transfer_handle *transfer) {
    int rem = len;
    int transferred = 0;
    xdma_transfer_handle tmpHandle = 0;
    xdma_status ret;
    pthread_mutex_lock(&_copyMutex);
    while (transferred < len) {
        int chunkSize = rem < MAX_DMA_TRANSFER_SIZE ? rem : MAX_DMA_TRANSFER_SIZE;
        ret = xdmaWaitTransfer(&tmpHandle);  //Wait for previous transfer to finish
        if (ret != XDMA_SUCCESS) break;
        xdmaMemcpyAsyncChunk(usr + transferred, buffer, chunkSize, offset + transferred, mode, &tmpHandle);
        if (ret != XDMA_SUCCESS) break;
        transferred += chunkSize;
        rem -= chunkSize;
    }
    pthread_mutex_unlock(&_copyMutex);
    *transfer = tmpHandle;
    return ret;
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
    if (_mem_fd > 0 && _dev_mem_fd > 0) return XDMA_SUCCESS;

    xdma_status ret = XDMA_ERROR;
    int errorCode;
    _mem_fd = open(MEM_FILEPATH, O_RDWR);
    if (_mem_fd == -1) {
        errorCode = errno;
        perror("Could not initialize kernel memory");
        goto mem_open_error;
    }
    _dev_mem_fd = open(DEV_MEM_FILEPATH, O_RDWR);
    if (_dev_mem_fd == -1) {
        errorCode = errno;
        perror("Could not open dev memory device");
        goto dev_mem_open_error;
    }

    _devMem = (uintptr_t)mmap(NULL, DEV_MEM_SIZE, PROT_READ | PROT_WRITE,
            MAP_SHARED, _dev_mem_fd, 0);
    if ((void *)_devMem == MAP_FAILED) {
        perror("Failed to map device memory");
        errorCode = errno;
        goto dev_mem_map_error;
    }
    _curDevMemPtr = _devMem;

    //Initialize mutex
    pthread_mutex_init(&_allocateMutex, NULL);
    pthread_mutex_init(&_copyMutex, NULL);
    return XDMA_SUCCESS;

dev_mem_map_error:
    close(_dev_mem_fd);
dev_mem_open_error:
    close(_mem_fd);
mem_open_error:
    switch (errorCode) {
        case EACCES: //User cannot access instrumentation device
            ret = XDMA_EACCES;
            break;
        case ENOENT: //Instrumentation not available
            ret = XDMA_ENOENT;
            break;
            //TODO add possible error codes
    }
    return ret;
}

xdma_status xdmaFiniMem() {
    if (_mem_fd > 0) {
        close(_mem_fd);
    }
    if (_dev_mem_fd > 0) {
        close(_dev_mem_fd);
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
    status = ioctl(_instr_fd, HWCOUNTER_GET_ADDR, &addr);
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
