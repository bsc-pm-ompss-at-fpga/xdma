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

typedef struct {
    struct {
        uint64_t timer;
        uint64_t buffer;
    } profile;
    uint32_t accId;
    uint32_t compute;
} xdma_task_header;


#define COPY_ID_WIDTH       24
#define CACHE_FLAGS_WIDTH   8
#define TASK_MAX_LEN        256 //Header + 19 args + 4 bytes (not used)

typedef struct __attribute__ ((__packed__)) {
    unsigned int cacheFlags: CACHE_FLAGS_WIDTH; //low 8bit
    unsigned int paramId: COPY_ID_WIDTH;        //high 24bit
    uint64_t address;
} xdma_arg;


//HW tracing stuff
//TODO: allow to add more counter buffers as needed

#define INSTRUMENT_NUM_COUNTERS     4
#define INSTRUMENT_BUFFER_SIZE      4096    //1 page
//#define INSTRUMENT_NUM_ENTRIES      (INSTRUMENT_BUFFER_SIZE/(INSTRUMENT_NUM_COUNTERS*sizeof(uint32_t)))
#define INSTRUMENT_NUM_ENTRIES       (INSTRUMENT_BUFFER_SIZE/sizeof(xdma_instr_times))
#define INSTRUMENT_PARAM_NUM        2

#define INSTRUMENT_HW_COUNTER_ADDR   0X80000000

uint64_t *instrumentBuffer;
uint64_t *instrumentPhyAddr;

xdma_buf_handle instrBufferHandle;

//FIXME instrument_entry structure may not be necessary
typedef struct {
    int taskID;             //not sure if needed
    uint64_t *counters;     //userspace counter addr
    uint64_t *phyCounters;  //physical counter addr
} xdma_instrument_entry;

xdma_instrument_entry instrumentEntries[INSTRUMENT_NUM_ENTRIES];

//Task descriptor buffers

void *taskDescriptors;
xdma_buf_handle taskDescriptorsHandle;

#define MAX_RUNNING_TASKS       INSTRUMENT_NUM_ENTRIES

struct task_entry {
    void *taskDescriptor;               //Pointer to the task descriptor
    size_t argsCnt;                     //Number of arguments in task
    xdma_buf_handle taskHandle;         //Task buffer handle
    xdma_transfer_handle descriptorTx;  //Task descriptor transfer handle
    xdma_transfer_handle syncTx;        //Task sync transfer handle
};

static struct task_entry taskEntries[MAX_RUNNING_TASKS];
//TODO: Constructor in order to initialize everything

//Allocate a buffer to send instrumentation parameters to a task (or hw transaction)
static uint64_t *instrParamBuffer;
static xdma_buf_handle instrParamHandle;

static xdma_status xdmaInitTasks() {
    return xdmaAllocateKernelBuffer(&taskDescriptors, &taskDescriptorsHandle,
            TASK_MAX_LEN*MAX_RUNNING_TASKS);
}

static xdma_status xdmaFiniTasks() {
    return xdmaFreeKernelBuffer(taskDescriptors, taskDescriptorsHandle);
}

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
        perror("Error opening file for writing");
        return XDMA_ERROR;
    }

    //Initialize mutex
    pthread_mutex_init(&_allocateMutex, NULL);

    //try to initialize instrumentation support
    // NOTE: Initialization disabled as instrumentation may be not-wanted
    //xdmaInitHWInstrumentation();

    //initialize tasking
    //Allocate space for the task descriptors in order not to allocate a new buffer
    //for each task
    xdmaInitTasks();

    //initialize devices
    int numDevices;
    xdmaGetNumDevices(&numDevices);
    xdma_device *devices;
    devices = (xdma_device*)alloca(numDevices*sizeof(xdma_device));
    xdmaGetDevices(numDevices, devices, NULL);
    for (int i=0; i<numDevices; i++) {
        //need te run channel configuration to initialize the channel table
        xdma_channel dummy_channel;
        xdmaOpenChannel(devices[i], XDMA_FROM_DEVICE, XDMA_CH_NONE, &dummy_channel);
        xdmaOpenChannel(devices[i], XDMA_TO_DEVICE, XDMA_CH_NONE, &dummy_channel);
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

    //Finalize tasking
    xdmaFiniTasks();

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
    *channel = (xdma_channel)ch_config;
    return XDMA_SUCCESS;
}

xdma_status xdmaCloseChannel(xdma_channel *channel) {
    //Not necessary at this point
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
    if (_instr_fd > 0) return XDMA_EISINIT;

    //Open instrumentation file
    _instr_fd = open(INSTR_FILEPATH, O_RDONLY);
    if (_instr_fd == -1) {
        perror("Error opening instrumentation device file for reading");
        return XDMA_ERROR;
    }

    //allocate instrumentation buffer & get its physical address
    xdmaAllocateKernelBuffer((void**)&instrumentBuffer, &instrBufferHandle,
            INSTRUMENT_BUFFER_SIZE);
    xdmaGetDMAAddress(instrBufferHandle, (unsigned long*)&instrumentPhyAddr);
    //Allocate parameter buffer
    xdmaAllocateKernelBuffer((void**)&instrParamBuffer, &instrParamHandle,
            INSTRUMENT_NUM_ENTRIES*INSTRUMENT_PARAM_NUM*sizeof(uint32_t));
    memset(instrumentEntries, 0, INSTRUMENT_NUM_ENTRIES*sizeof(xdma_instrument_entry));
    return XDMA_SUCCESS;
}

xdma_status xdmaFiniHWInstrumentation() {
    xdma_status sPH, sBH;
    instrumentPhyAddr = NULL;
    sPH = xdmaFreeKernelBuffer(instrParamBuffer, instrParamHandle);
    sBH = xdmaFreeKernelBuffer(instrumentBuffer, instrBufferHandle);
    if (_instr_fd > 0) {
        close(_instr_fd);
    }
    return sPH == XDMA_SUCCESS ? sBH : sPH;
}

static int getFreeEntry() {
    for (int i=0; i<INSTRUMENT_NUM_ENTRIES; i++) {
        if (instrumentEntries[i].taskID == 0) {
            if (__sync_add_and_fetch(&(instrumentEntries[i].taskID), 1 ) == 1) {
               return i;
            }
            __sync_sub_and_fetch(&(instrumentEntries[i].taskID), 1 );
        }
    }
    return -1;
}

xdma_status xdmaClearTaskTimes(xdma_instr_times *taskTimes) {
    //find the instrument entry
    memset(taskTimes, 0, sizeof(xdma_instr_times));
    for (int i=0; i<INSTRUMENT_NUM_ENTRIES; i++) {
        if (instrumentEntries[i].counters == (uint64_t*)taskTimes) {
            memset(&instrumentEntries[i], 0, sizeof(xdma_instrument_entry));
            return XDMA_SUCCESS;
        }
    }
    return XDMA_ERROR;
}

xdma_status xdmaGetDeviceTime(uint64_t *time) {
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

xdma_status xdmaInitTask(int accId, xdma_compute_flags compute, xdma_task_handle *taskDescriptor) {
    //TODO: Error checking
    //Get a destination buffer for instrumentation data
    int freeEntry;
    uint64_t instrAddress;
    freeEntry = getFreeEntry();
    if (freeEntry < 0) {
        return XDMA_ENOMEM;
    }
    instrAddress = (uint64_t)((uintptr_t)&instrumentPhyAddr[freeEntry*INSTRUMENT_NUM_COUNTERS]);
    //NOTE: Atomically done in the getInstrFreeEntry
    //instrumentEntries[freeEntry].taskID = 1;

    //Allocate task descriptor inside the kernel
    //  TODO: Reuse buffers
    xdma_task_header *taskHeader;
    taskHeader = taskDescriptors + freeEntry*TASK_MAX_LEN;
    taskEntries[freeEntry].taskDescriptor = (void *)taskHeader;
    taskEntries[freeEntry].taskHandle = taskDescriptorsHandle;
    taskEntries[freeEntry].argsCnt = 0;

    //Fill the task header structure
    taskHeader->profile.timer = INSTRUMENT_HW_COUNTER_ADDR;
    taskHeader->profile.buffer = instrAddress;
    taskHeader->accId = accId; //FIXME
    taskHeader->compute = compute;

    *taskDescriptor = freeEntry;
    return XDMA_SUCCESS;
}

xdma_status xdmaAddArg(xdma_task_handle taskHandle, size_t argId, xdma_mem_flags flags,
        xdma_buf_handle buffer, size_t offset) {
    void *task;
    xdma_arg *args;

    if (taskHandle < 0 || !buffer) return XDMA_EINVAL;

    //Add offset from the header
    task = taskEntries[taskHandle].taskDescriptor + sizeof(xdma_task_header);
    args = (xdma_arg*)task;
    //Get the physical address & apply offset
    unsigned long phyAddr;
    if (xdmaGetDMAAddress(buffer, &phyAddr) != XDMA_SUCCESS) {
        return XDMA_ERROR;
    }
    phyAddr += offset;

    //Fill the argument entry
    size_t idx = taskEntries[taskHandle].argsCnt++;
    args[idx].cacheFlags = flags;
    args[idx].paramId = argId;
    args[idx].address = (uint64_t)phyAddr;
    return XDMA_SUCCESS;
}

xdma_status xdmaSendTask(xdma_device dev, xdma_task_handle taskHandle) {
    //Send the task descriptor
    xdma_buf_handle taskBuffer;
    xdma_transfer_handle descHandle, syncHandle;
    xdma_status retD, retS;

    taskBuffer = taskEntries[taskHandle].taskHandle;
    xdma_task_header *taskHeader = (xdma_task_header*)taskEntries[taskHandle].taskDescriptor;
    size_t size = sizeof(xdma_task_header) + taskEntries[taskHandle].argsCnt*sizeof(xdma_arg);

    //get device's channels
    int devNumber = ((struct xdma_dev*)dev - _devices);
    //direction is going to be 0 or 1
    xdma_channel inChannel =
        (xdma_channel)&_channels[devNumber*CHANNELS_PER_DEVICE + XDMA_TO_DEVICE];
    xdma_channel outChannel =
        (xdma_channel)&_channels[devNumber*CHANNELS_PER_DEVICE + XDMA_FROM_DEVICE];

    unsigned int descOffset = (void *)taskHeader - taskDescriptors;
    retD = xdmaSubmitKBuffer(taskDescriptorsHandle, size, descOffset,
           XDMA_ASYNC, dev, inChannel, &descHandle);
    retS = xdmaSubmitKBuffer(taskBuffer, sizeof(uint32_t),
           descOffset + offsetof(xdma_task_header, compute),
           XDMA_ASYNC, dev, outChannel, &syncHandle);

    taskEntries[taskHandle].descriptorTx = descHandle;
    taskEntries[taskHandle].syncTx = syncHandle;

    return retD == XDMA_SUCCESS ? retS : retD;
}

xdma_status xdmaGetInstrumentData(xdma_task_handle task, xdma_instr_times **times) {
    *times = (xdma_instr_times*)&instrumentBuffer[task*INSTRUMENT_NUM_COUNTERS];
    return XDMA_SUCCESS;
}

xdma_status xdmaWaitTask(xdma_task_handle handle) {
    xdma_status retD, retS;
    retD = xdmaWaitTransfer(taskEntries[handle].descriptorTx);
    retS = xdmaWaitTransfer(taskEntries[handle].syncTx);
    return retD == XDMA_SUCCESS ? retS : retD;
}

xdma_status xdmaDeleteTask(xdma_task_handle *handle) {
    //Mark instrument entries as free
    memset(&instrumentEntries[*handle], 0, sizeof(xdma_instrument_entry));
    *handle = -1;
    return XDMA_SUCCESS;
}
