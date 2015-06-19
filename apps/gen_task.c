#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "libxdma.h"
#include "timing.h"

#define MAX_DMA_DEVICES 2
#define OUT_REF_VAL 0xDEADBEEF
#define NUMDEVS_ENV "NUM_DMA_DEVICES"

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

    xdmaAllocateKernelBuffer((void**)&inData, inLen*sizeof(int));
    xdmaAllocateKernelBuffer((void**)&outData, outLen*sizeof(int));
    xdmaAllocateKernelBuffer((void**)&args, sizeof(struct args_t));
    xdmaAllocateKernelBuffer((void**)&waited, sizeof(int));

    xdma_device devices[MAX_DMA_DEVICES];
    //devices = (xdma_device)malloc(ndevs*sizeof(xdma_device));
    xdmaGetDevices(ndevs, devices, NULL);

    xdma_channel inChannel[MAX_DMA_DEVICES], outChannel[MAX_DMA_DEVICES];
    //xdma_status xdmaOpenChannel(xdma_device device, xdma_dir direction, unsigned int flags, xdma_channel *channel) {
    for (int i=0; i<ndevs; i++) {
        xdmaOpenChannel(devices[i], XDMA_TO_DEVICE, 0, &inChannel[i]);
        xdmaOpenChannel(devices[i], XDMA_FROM_DEVICE, 0, &outChannel[i]);
    }

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
            outData[i] = 0;
        }
        int devIndex = ii%ndevs;
        *waited = 0;
        args->in = inLen;
        args->wait = wait;
        args->out = outLen;

        start = getusec_();

        //send buffers
        xdma_transfer_handle inTrans, outTrans, argTrans, waitedTrans;
        //xdma_status xdmaSubmitKBuffer(void *buffer, size_t len, int wait, xdma_device dev, xdma_channel channel,
        //        xdma_transfer_handle *transfer);
        xdmaSubmitKBuffer(args, sizeof(struct args_t), XDMA_ASYNC, devices[devIndex], inChannel[devIndex], &argTrans);
        xdmaSubmitKBuffer(inData, inLen*sizeof(int), XDMA_ASYNC, devices[devIndex], inChannel[devIndex], &inTrans);
        xdmaSubmitKBuffer(waited, sizeof(int), XDMA_ASYNC, devices[devIndex], outChannel[devIndex], &waitedTrans);
        xdmaSubmitKBuffer(outData, outLen*sizeof(int), XDMA_ASYNC, devices[devIndex], outChannel[devIndex], &outTrans);

        //wait for the transfers
        xdmaWaitTransfer(argTrans);
        xdmaWaitTransfer(inTrans);
        xdmaWaitTransfer(waitedTrans);
        xdmaWaitTransfer(outTrans);

        xdmaReleaseTransfer(&argTrans);
        xdmaReleaseTransfer(&inTrans);
        xdmaReleaseTransfer(&waitedTrans);
        xdmaReleaseTransfer(&outTrans);
        time += getusec_() - start;

        //check results
        if (*waited != wait) {
            fprintf(stderr, "Error checking waited cycles for iteration %d (%d instead of %d)\n",
                    ii, *waited, wait);
            errors++;
        }
        for (int i=0; i<outLen; i++) {
            if (outData[i] != OUT_REF_VAL) {
                fprintf(stderr, "Error in output data #%d in iterarion $d: %d instead of %d\n",
                        i, outData[i], OUT_REF_VAL);
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
