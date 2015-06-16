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

#define BUS_IN_BYTES 4
#define BUS_BURST 16
//Assume there only are 1 in + 1 out channel per device
#define CHANNELS_PER_DEVICE 2
#define MAX_CHANNELS        MAX_DEVICES*CHANNELS_PER_DEVICE

static int _fd;
static uint8_t *_map;		/* mmapped array of char's */

static int _numDevices;
static struct xdma_dev _devices[MAX_DEVICES];
static struct xdma_chan_cfg _channels[MAX_CHANNELS];
static unsigned int _kUsedSpace;


static int getDeviceInfo(int deviceId, struct xdma_dev *devInfo);


//TODO: Constructor in order to initialize everything

xdma_status xdmaOpen() {
    _numDevices = -1;
    _kUsedSpace = 0;
    //TODO: check if library has been initialized
    _fd = open(FILEPATH, O_RDWR | O_CREAT | O_TRUNC, (mode_t) 0600);
    if (_fd == -1) {
        perror("Error opening file for writing");
        return XDMA_ERROR;
    }

    _map = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, 0);
    if (_map == MAP_FAILED) {
        close(_fd);
        perror("Error mmapping the file");
        return XDMA_ERROR;
    }
    return XDMA_SUCCESS;
}

xdma_status xdmaClose() {
    if (munmap(_map, MAP_SIZE) == -1) {
        perror("Error un-mmapping kernel memory");
        return XDMA_ERROR;
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
            return XDMA_ERROR;
        }
        //Initialize all devices
        for (int i=0; i<ndevs; i++) {
            status = getDeviceInfo(i, &_devices[i]);
            if (status != XDMA_SUCCESS) {
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
            devices[i] = (xdma_device) &_devices[i];
        }
    }
    if (devs) {
        *devs = retDevs;
    }

    return XDMA_SUCCESS;
}

xdma_status xdmaOpenChannel(xdma_device device, xdma_dir direction, xdma_channel_flags flags, xdma_channel *channel) {
    //Already initialized channels are not checked
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

    //These should be configurable using flags (TODO)
    ch_config->coalesc = 1;
    ch_config->delay = 0;
    ch_config->reset = 0;
    if (ioctl(_fd, XDMA_DEVICE_CONTROL, ch_config) < 0) {
        perror("Error ioctl config rx chan");
        return XDMA_ERROR;
    }
    *channel = (xdma_channel)ch_config;
    return XDMA_SUCCESS;
}

xdma_status xdmaCloseChannel(xdma_channel *channel) {
    //Not necessary at this point
    return XDMA_SUCCESS;
}

xdma_status xdmaAllocateKernelBuffer(void **buffer, size_t len) {
    if (len > MAP_SIZE - _kUsedSpace) {
        //if there is not eough left space
        fprintf(stderr, "Error allocating kernel buffer\n");
        return XDMA_ERROR;
    }
    *buffer = _map + _kUsedSpace;
    _kUsedSpace += len;
    return XDMA_SUCCESS;
}

xdma_status xdmaFreeKernelBuffers() {
    _kUsedSpace = 0;
    return XDMA_SUCCESS;
}

xdma_status xdmaSubmitKBuffer(void *buffer, size_t len, xdma_xfer_mode mode, xdma_device dev, xdma_channel ch, xdma_transfer_handle *transfer) {

    struct xdma_chan_cfg *channel = (struct xdma_chan_cfg*)ch;
    struct xdma_dev *device = (struct xdma_dev*)dev;
    //prepare kernel mapped buffer & submit
    struct xdma_buf_info buf;
    buf.chan = channel->chan;
    //Weird. Completion callback may be stored per channel
    buf.completion =
        (channel->dir == XDMA_DEV_TO_MEM) ? device->rx_cmp : device->tx_cmp;
    buf.cookie = (u32) NULL;
    buf.buf_offset = (u32) ((uint8_t *)buffer - _map);
    buf.buf_size = (u32) len;
    buf.dir = channel->dir;

    if (ioctl(_fd, XDMA_PREP_BUF, &buf) < 0) {
        perror("Error ioctl set rx buf");
        return XDMA_ERROR;
    }

//    //prepare input buffer
//    struct xdma_buf_info tx_buf;
//    tx_buf.chan = dev.tx_chan;
//    tx_buf.completion = dev.tx_cmp;
//    tx_buf.cookie = (u32) NULL;
//    tx_buf.buf_offset = (u32) in_offset;
//    tx_buf.buf_size = (u32) in_len;
//    tx_buf.dir = XDMA_MEM_TO_DEV;
//    if (ioctl(fd, XDMA_PREP_BUF, &tx_buf) < 0) {
//        perror("Error ioctl set tx buf");
//        return -1;
//    }

    //start input transfer host->dev
    struct xdma_transfer *trans;
    //May not be a good thing for the performance to allocate things for each
    //transfer, but I did not come up with anythong better
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
    if (ioctl(_fd, XDMA_START_TRANSFER, trans) < 0) {
        perror("Error ioctl start tx trans");
        return XDMA_ERROR;
    }
    if (XDMA_ASYNC) {
        *transfer = (xdma_transfer_handle)trans;
    }
    return XDMA_SUCCESS;

//    //start output transfer dev->host
//    struct xdma_transfer rx_trans;
//    rx_trans.chan = dev.rx_chan;
//    rx_trans.completion = dev.rx_cmp;
//    rx_trans.cookie = rx_buf.cookie;
//    rx_trans.wait = 0;
//    if (ioctl(fd, XDMA_START_TRANSFER, &rx_trans) < 0) {
//        perror("Error ioctl start rx trans");
//        return -1;
//    }
}

xdma_status xdmaFinishTransfer(xdma_transfer_handle *transfer, int wait) {
    struct xdma_transfer *trans = (struct xdma_transfer *)transfer;
    int status;

    if (!trans) {
        //If the pointer is null we assume that the transfer has been sucessfully
        //completed and freed
        return XDMA_SUCCESS;
    }

    trans->wait = (wait != 0);

    status = ioctl(_fd, XDMA_FINISH_TRANSFER, trans);
    if (status < 0) {
        perror("Transfer finish error\n");
        return XDMA_ERROR;
    } else if (status == XDMA_DMA_TRANSFER_PENDING) {
        return XDMA_PENDING;
    }
    //free the transfer data structure since it has finished and it's not needed anymore

    free(trans);
    *transfer = (xdma_transfer_handle)NULL;

    //TODO: reuse allocated transfer structures
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

    if (!trans) {
        return XDMA_SUCCESS;
    }

    free(trans);
    *transfer = (xdma_transfer_handle)NULL;

    //TODO: reuse allocated transfer structures
    return XDMA_SUCCESS;
}

static int getDeviceInfo(int deviceId, struct xdma_dev *devInfo) {
    devInfo->tx_chan = (u32) NULL;
    devInfo->tx_cmp = (u32) NULL;
    devInfo->rx_chan = (u32) NULL;
    devInfo->rx_cmp = (u32) NULL;
    devInfo->device_id = deviceId;
    if (ioctl(_fd, XDMA_GET_DEV_INFO, devInfo) < 0) {
        perror("Error ioctl getting device info");
        return XDMA_ERROR;
    }
    return XDMA_SUCCESS;
}

