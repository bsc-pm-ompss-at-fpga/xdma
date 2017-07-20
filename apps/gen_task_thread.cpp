#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <queue>

#include <pthread.h>

#include "libxdma.h"
#include "timing.h"

#define MAX_DMA_DEVICES 2
#define PIPELINE_ENV "DMA_PIPELINE_LEN"
#define NUM_THREADS_ENV "GENTASK_THREADS"
#define MIN_PIPELINE_LEN 2
#define MAX_NUM_THREADS 2
#define DEFAULT_NUM_THREADS 1

const int IN_MAGIC_VAL = 0xC0FFEE;
const int OUT_REF_VAL  = 0xDEADBEEF;

#define WAIT_ALL -1
static unsigned int _pipeline = 32;


inline void waitTransfers(std::queue<xdma_transfer_handle> &q, int num) {
    int size = q.size();
    num = (num<0 || num> size) ? size : num;
    for (int i=0; i<num; i++) {
        xdma_transfer_handle tx = q.front();
        xdmaWaitTransfer(tx);
        xdmaReleaseTransfer(&tx);
        q.pop();
    }
}

inline void pushTransfer( std::queue<xdma_transfer_handle> &q, xdma_transfer_handle handle) {
    if (q.size() >= _pipeline) {
        waitTransfers(q, 1);
    }
    q.push(handle);
}

//This is used to transfer the arguments in the correct order
//They need to be transferred in the correct order
struct args_t {
    int in;
    int wait;
    int out;
};

struct thread_args_t {
    int in;
    int wait;
    int out;
    int *in_base;
    int *waited_base;
    int *out_base;
    args_t *taskArgs;
    xdma_device device;
    int iter;
};

void *genTaskThread(void *tArgs) {
    thread_args_t *args = (thread_args_t*)tArgs;
    xdma_channel inChannel, outChannel;

    xdmaGetDeviceChannel(args->device, XDMA_TO_DEVICE, &inChannel);
    xdmaGetDeviceChannel(args->device, XDMA_FROM_DEVICE, &outChannel);

    std::queue<xdma_transfer_handle> inQueue;
    std::queue<xdma_transfer_handle> outQueue;

    int iter = args->iter;

    for (int i=0; i<iter; i++) {
        xdma_transfer_handle argTrans, inTrans, waitedTrans, outTrans;
        xdmaSubmitKBuffer((void*)args->taskArgs, sizeof(struct args_t), XDMA_ASYNC, args->device, inChannel, &argTrans);
        pushTransfer(inQueue, argTrans);
        xdmaSubmitKBuffer(&args->in_base[i*args->in], args->in*sizeof(int), XDMA_ASYNC,
                args->device, inChannel, &inTrans);
        pushTransfer(inQueue, inTrans);
        xdmaSubmitKBuffer(&args->waited_base[i], sizeof(int), XDMA_ASYNC, args->device, outChannel, &waitedTrans);
        pushTransfer(outQueue, waitedTrans);
        xdmaSubmitKBuffer(&args->out_base[i*args->out], args->out*sizeof(int), XDMA_ASYNC,
                args->device, outChannel, &outTrans);
    }
    waitTransfers(inQueue, WAIT_ALL);
    waitTransfers(outQueue, WAIT_ALL);

    return NULL;
}

int main(int argc, char **argv) {
    int inLen, wait, outLen, iter;
    if (argc > 4) {
        inLen = atoi(argv[1]);
        wait = atoi(argv[2]);
        outLen = atoi(argv[3]);
        iter = atoi(argv[4]);
    } else {
        fprintf(stderr, "usage: %s <in_data> <wait_cycles> <out_data> <iteration>\n"
                "Note that data length is specified as 32-bit elements\n", argv[0]);
        exit(1);
    }

    int ndevs;
    //char *devEnv;

    xdmaOpen();
    xdmaGetNumDevices(&ndevs);
//    devEnv = getenv(NUMDEVS_ENV);
//    if (!devEnv) {
//        ndevs = 1;
//    } else {
//        int ndevEnv = atoi(devEnv);
//        ndevs = (ndevEnv < ndevs) ? ndevEnv : ndevs;
//    }

    char *pplineEnv;
    pplineEnv = getenv(PIPELINE_ENV);
    if (pplineEnv) {
        int p;
        p = atoi(pplineEnv);
        if (p < 2) {
            fprintf(stderr, "Pipeline length cannot be <2. Defaulting to 2");
            _pipeline = 2;
        } else {
            _pipeline = p;
        }
    }
    int nThreads;
    char *nThreadEnv;
    nThreadEnv = getenv(NUM_THREADS_ENV);
    if (nThreadEnv) {
        nThreads = atoi(nThreadEnv);
        nThreads = nThreads > 0 ? nThreads : DEFAULT_NUM_THREADS;
    } else {
        nThreads = DEFAULT_NUM_THREADS; //1
    }

    //TODO: Check if there are actually enough accelerators for every thread
    ndevs = nThreads; //1 acc per thread
    int *inData, *outData;
    int *waited;
    args_t *taskArgs;
    thread_args_t args[MAX_NUM_THREADS];
    pthread_t threads[MAX_NUM_THREADS];

    xdmaAllocateKernelBuffer((void**)&inData, inLen*sizeof(int)*iter);
    xdmaAllocateKernelBuffer((void**)&outData, outLen*sizeof(int)*iter);
    xdmaAllocateKernelBuffer((void**)&taskArgs, sizeof(struct args_t));
    xdmaAllocateKernelBuffer((void**)&waited, sizeof(int)*iter);

    xdma_device devices[MAX_DMA_DEVICES];
    xdmaGetDevices(ndevs, devices, NULL);

    //check that more threads than devices are not created

    //init data
    for (int i=0; i<iter; i++) {
        waited[i] = 0;
    }
    for (int i=0; i<inLen*iter; i++) {
        inData[i] = IN_MAGIC_VAL;
    }
    for (int i=0; i<outLen*iter; i++) {
        outData[i] = 0;
    }

    //Initialize task argument once as it's shared between all threads
    taskArgs->in = inLen;
    taskArgs->wait = wait;
    taskArgs->out = outLen;

    double start, time;
    int errors = 0;
    start = getusec_();
    for (int i=0; i<nThreads; i++) {
        //set args
        int ipt = iter/nThreads;
        args[i].in = inLen;
        args[i].wait = wait;
        args[i].out = outLen;
        args[i].in_base = &inData[inLen*ipt*i];
        args[i].waited_base = &waited[ipt*i];
        args[i].out_base = &outData[outLen*ipt*i];
        args[i].taskArgs = taskArgs;
        args[i].device = devices[i];
        args[i].iter = ipt;

        pthread_create(&threads[i], NULL, genTaskThread, (void*)&args[i]);
    }

    for (int i=0; i<nThreads; i++) {
        pthread_join(threads[i], NULL);
    }


    time = getusec_() - start;

    for (int ii=0; ii<iter; ii++) {
        if (waited[ii] != wait) {
            fprintf(stderr, "Error checking waited cycles for iteration %d (%d instead of %d)\n",
                    ii, waited[ii], wait);
            if (waited[ii] < 0) {
                fprintf(stderr, "    %d elements failed to read from the accelerator\n", -waited[ii]);
            }
            errors++;
        }
        for (int i=0; i<outLen; i++) {
            if (outData[ii*outLen + i] != (int)OUT_REF_VAL) {
                fprintf(stderr, "Error in output data #%d in iterarion %d: %d instead of %d\n",
                        i, ii, outData[i], OUT_REF_VAL);
                errors++;
            }
        }
    }
    printf("%lf\n", time/1e6);

    xdmaClose();
    int ret;
    if (errors) {
        printf("%d errors found!!\n"
                "FAIL\n", errors);
        ret = 1;
    } else {
        ret = 0;
    }
    return ret;
}
