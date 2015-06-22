#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <queue>

#include "libxdma.h"
#include "timing.h"

#define MAX_DMA_DEVICES 2
#define OUT_REF_VAL 0xDEADBEEF
#define NUMDEVS_ENV "NUM_DMA_DEVICES"

#define WAIT_ALL -1
#define PIPELINE 32

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
    if (q.size() >= PIPELINE) {
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

    int *inData, *outData;
    int *waited;
    struct args_t *args;

    xdmaAllocateKernelBuffer((void**)&inData, inLen*sizeof(int)*iter);
    xdmaAllocateKernelBuffer((void**)&outData, outLen*sizeof(int)*iter);
    xdmaAllocateKernelBuffer((void**)&args, sizeof(struct args_t));
    xdmaAllocateKernelBuffer((void**)&waited, sizeof(int)*iter);

    xdma_device devices[MAX_DMA_DEVICES];
    //devices = (xdma_device)malloc(ndevs*sizeof(xdma_device));
    xdmaGetDevices(ndevs, devices, NULL);

    xdma_channel inChannel[MAX_DMA_DEVICES], outChannel[MAX_DMA_DEVICES];
    //xdma_status xdmaOpenChannel(xdma_device device, xdma_dir direction, unsigned int flags, xdma_channel *channel) {
    for (int i=0; i<ndevs; i++) {
        xdmaOpenChannel(devices[i], XDMA_TO_DEVICE, XDMA_CH_NONE, &inChannel[i]);
        xdmaOpenChannel(devices[i], XDMA_FROM_DEVICE, XDMA_CH_NONE, &outChannel[i]);
    }
    std::queue<xdma_transfer_handle> inQueue;
    std::queue<xdma_transfer_handle> outQueue;

    double start, time;
    time = 0.0;
    int errors = 0;
    for (int ii=0; ii<iter; ii++) {
        //init data

        //Dont't need to initialize input data
        //for (int i=0; i<inLen; i++) {
        //    inData[i] = -1;
        //}

        for (int i=0; i<outLen; i++) {
            outData[ii*outLen + i] = 0;
        }
        int devIndex = ii%ndevs;
        waited[ii] = 0;
        args->in = inLen;
        args->wait = wait;
        args->out = outLen;


        start = getusec_();

        //send buffers
        xdma_transfer_handle inTrans, outTrans, argTrans, waitedTrans;
        //xdma_status xdmaSubmitKBuffer(void *buffer, size_t len, int wait, xdma_device dev, xdma_channel channel,
        //        xdma_transfer_handle *transfer);
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

        time += getusec_() - start;


    }
#ifndef NO_PIPELINE
    waitTransfers(inQueue, WAIT_ALL);
    waitTransfers(outQueue, WAIT_ALL);
#endif // !NO_PIPELINE

    for (int ii=0; ii<iter; ii++) {
        if (waited[ii] != wait) {
            fprintf(stderr, "Error checking waited cycles for iteration %d (%d instead of %d)\n",
                    ii, *waited, wait);
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
    //free(devices);
    int ret;
    if (errors) {
        printf("%d errors found!!\n"
                "FAIL\n", errors);
        ret = 1;
    } else {
        printf("PASS\n");
        ret = 0;
    }
    return ret;
}
