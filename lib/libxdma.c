#include "libxdma.h"

// the below defines are a hack that enables the use of kernel data types
// without having to included standard kernel headers
#define u32 uint32_t
// dma_cookie_t is defined in the kernel header <linux/dmaengine.h>
#define dma_cookie_t int32_t
#include "xdma.h"
#include "libxdma.h"

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

#define BUS_IN_BYTES 4
#define BUS_BURST 16
//Assume there only are 1 in + 1 out channel per device
#define CHANNELS_PER_DEVICE 2
#define MAX_CHANNELS        MAX_DEVICES*CHANNELS_PER_DEVICE

static int _fd;
static int _instr_fd;

static int _numDevices;
static struct xdma_dev _devices[MAX_DEVICES];
static struct xdma_chan_cfg _channels[MAX_CHANNELS];
static unsigned int _kUsedSpace;
static pthread_mutex_t _allocateMutex;
static pthread_mutex_t *_submitMutexes;
static int _open_cnt = 0;

static int getDeviceInfo(int deviceId, struct xdma_dev *devInfo);
static xdma_status xdmaOpenChannel(xdma_device device, xdma_dir direction);

#define INSTRUMENT_HW_COUNTER_ADDR   0X80000000

xdma_status xdmaOpen() {
    int open_cnt;

    // Handle multiple opens
    open_cnt = __sync_fetch_and_add(&_open_cnt, 1);
    if (open_cnt > 0) return XDMA_SUCCESS;

    _numDevices = -1;
    _kUsedSpace = 0;
    _instr_fd = 0;
    //TODO: check if library has been initialized
    _fd = open(FILEPATH, O_RDWR | O_CREAT | O_TRUNC, (mode_t) 0600);
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
            case default;
                break;
        }
        return ret;
    }

    //Initialize mutex
    pthread_mutex_init(&_allocateMutex, NULL);

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

    //Mutex finalization
    pthread_mutex_destroy(&_allocateMutex);

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

xdma_status xdmaAllocateKernelBuffer(void **buffer, xdma_buf_handle *handle, size_t len) {
    //TODO: Check that mmap + ioctl are performet atomically
    unsigned long ret;
    unsigned int status;
    pthread_mutex_lock(&_allocateMutex);
    *buffer = mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, _fd, 0);
    if (*buffer == MAP_FAILED) {
        pthread_mutex_unlock(&_allocateMutex);
        perror("Error allocating kernel buffer: mmap failed");
        //TODO: return proper error codes (ENOMEM, etc)
        return XDMA_ERROR;
    }
    //get the handle for the allocated buffer
    status = ioctl(_fd, XDMA_GET_LAST_KBUF, &ret);
    pthread_mutex_unlock(&_allocateMutex);
    if (status) {
        perror("Error allocating pinned memory");
        munmap(buffer, len);
        return XDMA_ERROR;
    }
    *handle = (xdma_buf_handle)ret;
    return XDMA_SUCCESS;
}

xdma_status xdmaFreeKernelBuffer(void *buffer, xdma_buf_handle handle) {

    int size;
    size = ioctl(_fd, XDMA_RELEASE_KBUF, &handle);
    if (size <= 0) {
        perror("could not release pinned buffer");
        return XDMA_ERROR;
    }

    if (munmap(buffer, size)) {
        perror("Failed to unmap pinned buffer");
        return XDMA_ERROR;
    }
    return XDMA_SUCCESS;
}

xdma_status xdmaSubmitKBuffer(xdma_buf_handle buffer, size_t len, unsigned int offset,
        xdma_xfer_mode mode, xdma_device dev, xdma_channel ch,
        xdma_transfer_handle *transfer) {

    struct xdma_chan_cfg *channel = (struct xdma_chan_cfg*)ch;
    struct xdma_dev *device = (struct xdma_dev*)dev;
    //prepare kernel mapped buffer & submit
    struct xdma_buf_info buf;
    buf.chan = channel->chan;
    //Weird. Completion callback may be stored per channel
    buf.completion =
        (channel->dir == XDMA_DEV_TO_MEM) ? device->rx_cmp : device->tx_cmp;
    buf.cookie = (dma_cookie_t)0;
    //Use address to store the buffer descriptor handle
    buf.address = (unsigned long)buffer;
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
    if (mode == XDMA_SYNC) {
        //If we are waiting for the transfer to finish, we can allocate
        //the data structure in the stack
        trans = (struct xdma_transfer*)alloca(sizeof(struct xdma_transfer));
    } else {
        trans = (struct xdma_transfer*)malloc(sizeof(struct xdma_transfer));
    }
    trans->chan = channel->chan;
    trans->completion = buf.completion;
    trans->cookie = buf.cookie;
    trans->wait = mode & XDMA_SYNC; //XDMA_SYNC == 1
    trans->sg_transfer = 0;
    if (ioctl(_fd, XDMA_START_TRANSFER, trans) < 0) {
        pthread_mutex_unlock(devMutex);
        perror("Error ioctl start tx trans");
        return XDMA_ERROR;
    }
    pthread_mutex_unlock(devMutex);
    if (mode == XDMA_ASYNC) {
        *transfer = (xdma_transfer_handle)trans;
    }
    return XDMA_SUCCESS;
}


xdma_status xdmaSubmitBuffer(void *buffer, size_t len, xdma_xfer_mode mode, xdma_device dev,
        xdma_channel chan, xdma_transfer_handle *transfer) {
    struct xdma_transfer *tx = (struct xdma_transfer *)transfer;
    struct xdma_chan_cfg *channel = (struct xdma_chan_cfg*)chan;
    struct xdma_dev *device = (struct xdma_dev*)dev;
    struct xdma_buf_info buf;
    buf.chan = channel->chan;
    buf.completion =
        (channel->dir == XDMA_DEV_TO_MEM) ? device->rx_cmp : device->tx_cmp;
    buf.cookie = 0;
    buf.address = (unsigned long)buffer;
    buf.buf_size = (u32) len;
    buf.dir = channel->dir;

    if (ioctl(_fd, XDMA_PREP_USR_BUF, &buf) < 0) {
        perror("Error submitting userspace buffer");
        return XDMA_ERROR;
    }

    //start transfer
    //TODO: Refactor code in order to reuse the transfer start&wait between this and
    //xdmaSubmitKbuffer
    if (mode == XDMA_SYNC) {
        tx = (struct xdma_transfer*)alloca(sizeof(struct xdma_transfer));
    } else {
        tx = (struct xdma_transfer*)malloc(sizeof(struct xdma_transfer));
    }
    tx->chan = channel->chan;
    tx->completion = buf.completion;
    tx->cookie = (u32) buf.cookie;
    tx->wait = mode & XDMA_SYNC;
    tx->sg_transfer = buf.sg_transfer;
    if (ioctl(_fd, XDMA_START_TRANSFER, tx) < 0) {
        perror("Error starting SG transfer");
        if (mode == XDMA_ASYNC) {
            free(tx);
        }
        return XDMA_ERROR;
    }

    if (mode == XDMA_ASYNC) {
        *transfer = (xdma_transfer_handle)tx;
    }
    return XDMA_SUCCESS;
}

static inline xdma_status _xdmaFinishTransfer(xdma_transfer_handle transfer, xdma_xfer_mode mode) {
    struct xdma_transfer *trans = (struct xdma_transfer *)transfer;
    int status;

    if (!trans) {
        //Uninitialized/cleaned up transfer
        return XDMA_ERROR;
    }

    trans->wait = mode & XDMA_SYNC; //XDMA_SYNC == 1
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

xdma_status xdmaTestTransfer(xdma_transfer_handle transfer){
    return _xdmaFinishTransfer(transfer, 0);
}

xdma_status xdmaWaitTransfer(xdma_transfer_handle transfer){
    xdma_status status = _xdmaFinishTransfer(transfer, 1);
    if (status == XDMA_PENDING) {
        fprintf(stderr, "Warining: transfer %x timed out\n", (unsigned int)transfer);
    }
    return status;
}

xdma_status xdmaReleaseTransfer(xdma_transfer_handle *transfer){
    struct xdma_transfer *trans = (struct xdma_transfer *)*transfer;
    int status;

    if (!trans) {
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

xdma_status xdmaGetDMAAddress(xdma_buf_handle buffer, unsigned long *dmaAddress) {
    union {
        xdma_buf_handle dmaBuffer;
        unsigned long dmaAddress;
    } kArg;
    int status;
    kArg.dmaBuffer = buffer;
    status = ioctl(_fd, XDMA_GET_DMA_ADDRESS, &kArg);
    if (status) {
        perror("Error getting DMA address");
        return XDMA_ERROR;
    } else {
        *dmaAddress = kArg.dmaAddress;
        return XDMA_SUCCESS;
    }
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
            case default;
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

unsigned long xdmaGetInstrumentationTimerAddr() {
    return INSTRUMENT_HW_COUNTER_ADDR;
}
