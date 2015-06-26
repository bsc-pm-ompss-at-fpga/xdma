#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <queue>

#include "libxdma.h"
#include "timing.h"

#define MAX_DMA_DEVICES 2
#define NUMDEVS_ENV "NUM_DMA_DEVICES"
#define PIPELINE_ENV "DMA_PIPELINE_LEN"
#define MIN_PIPELINE_LEN 2

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
    char *devEnv;

    xdmaOpen();
    xdmaGetNumDevices(&ndevs);
    devEnv = getenv(NUMDEVS_ENV);
    if (!devEnv) {
        ndevs = 1;
    } else {
        int ndevEnv = atoi(devEnv);
        ndevs = (ndevEnv < ndevs) ? ndevEnv : ndevs;
    }

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

    int *inData, *outData;
    int *waited;
    struct args_t *args;

    xdmaAllocateKernelBuffer((void**)&inData, inLen*sizeof(int)*iter);
    xdmaAllocateKernelBuffer((void**)&outData, outLen*sizeof(int)*iter);
    xdmaAllocateKernelBuffer((void**)&args, sizeof(struct args_t));
    xdmaAllocateKernelBuffer((void**)&waited, sizeof(int)*iter);

    xdma_device devices[MAX_DMA_DEVICES];
    xdmaGetDevices(ndevs, devices, NULL);

    xdma_channel inChannel[MAX_DMA_DEVICES], outChannel[MAX_DMA_DEVICES];
    for (int i=0; i<ndevs; i++) {
        xdmaOpenChannel(devices[i], XDMA_TO_DEVICE, XDMA_CH_NONE, &inChannel[i]);
        xdmaOpenChannel(devices[i], XDMA_FROM_DEVICE, XDMA_CH_NONE, &outChannel[i]);
    }
    std::queue<xdma_transfer_handle> inQueue;
    std::queue<xdma_transfer_handle> outQueue;

    args->in = inLen;
    args->wait = wait;
    args->out = outLen;

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

    double start, time;
    int errors = 0;
    start = getusec_();
    for (int ii=0; ii<iter; ii++) {

        int devIndex = ii%ndevs;

        //send buffers
        xdma_transfer_handle inTrans, outTrans, argTrans, waitedTrans;
        xdmaSubmitKBuffer(args, sizeof(struct args_t), XDMA_ASYNC, devices[devIndex], inChannel[devIndex], &argTrans);
        pushTransfer(inQueue, argTrans);
        xdmaSubmitKBuffer(&inData[ii*inLen], inLen*sizeof(int), XDMA_ASYNC, devices[devIndex], inChannel[devIndex], &inTrans);
        pushTransfer(inQueue, inTrans);
        xdmaSubmitKBuffer(&waited[ii], sizeof(int), XDMA_ASYNC, devices[devIndex], outChannel[devIndex], &waitedTrans);
        pushTransfer(outQueue, waitedTrans);
        xdmaSubmitKBuffer(&outData[ii*outLen], outLen*sizeof(int), XDMA_ASYNC, devices[devIndex], outChannel[devIndex], &outTrans);
        pushTransfer(outQueue, outTrans);

#ifdef NO_PIPELINE
        waitTransfers(inQueue, WAIT_ALL);
        waitTransfers(outQueue, WAIT_ALL);
#endif //NO_PIPELINE
    }
#ifndef NO_PIPELINE
    waitTransfers(inQueue, WAIT_ALL);
    waitTransfers(outQueue, WAIT_ALL);
#endif // !NO_PIPELINE

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
