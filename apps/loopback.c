#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "libxdma.h"

#define TEST_VAL        0xBADC0FEE
#define MAX_ACC         2
#define DEFAULT_ACC     0

const int len = 16;

void usage(char *exe) {
    printf("Usage: %s <accelerator number>", exe);
}

int main(int argc, char *argv[]) {
    int acc;
    xdma_status status;
    if (argc < 2) {
        acc = DEFAULT_ACC;
    } else if (!strcmp(argv[1], "--help")) {
        usage(argv[0]);
        return 0;
    } else {
        acc = atoi(argv[1]);
        if (acc > MAX_ACC || acc < 0) {
            fprintf(stderr, "Warning:Wrong accelerator number, using first available\n");
            acc = DEFAULT_ACC;
        }
    }
    //using 1 accelerator

    status = xdmaOpen();
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error initializing DMA\n");
        exit(1);
    }

    int *inData, *outData;
    xdma_buf_handle inHandle, outHandle;
    //Buffers must be allocated in kernel space mapped memory
    xdmaAllocateKernelBuffer((void **)&inData, &inHandle, len*sizeof(int));
    xdmaAllocateKernelBuffer((void **)&outData, &outHandle, len*sizeof(int));

    for (int i=0; i<len; i++) {
        inData[i] = 0xC0000000 | i;
        outData[i] = 0;
    }

    xdma_device dev, devices[MAX_ACC];
    int accFound;
    status = xdmaGetDevices(MAX_ACC, devices, &accFound);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error getting platform devices\n");
        exit(1);
    }
    if (accFound-1 < acc) {
        fprintf(stderr, "Warning: trying to use acc %d, but only %d found\n"
                "Using acc 0\n", acc, accFound);
        acc = DEFAULT_ACC;
    }
    dev = devices[acc];
    xdma_channel inChannel, outChannel;
    status = xdmaOpenChannel(dev, XDMA_TO_DEVICE, XDMA_CH_NONE, &inChannel);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error opening input channel\n");
        exit(1);
    }
    status = xdmaOpenChannel(dev, XDMA_FROM_DEVICE, XDMA_CH_NONE, &outChannel);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error opening output channel\n");
        exit(1);
    }

    xdma_transfer_handle inTransfer, outTransfer;
    status = xdmaSubmitKBuffer(inHandle, len*sizeof(int), 0, XDMA_ASYNC, dev, inChannel,
            &inTransfer);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error submitting input transfer\n");
    }
    status = xdmaSubmitKBuffer(outHandle, len*sizeof(int), 0, XDMA_ASYNC, dev, outChannel,
            &outTransfer);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error submitting output transfer\n");
    }
    status = xdmaWaitTransfer(inTransfer);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error during input transfer finalization");
    }
    status = xdmaWaitTransfer(outTransfer);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error during output transfer finalization");
    }

    xdmaReleaseTransfer(&inTransfer);
    xdmaReleaseTransfer(&outTransfer);

    //validate results
    int errors = 0;
    for (int i=0; i<len; i++) {
        if (outData[i] != inData[i]) {
            printf("Unexpected output at position %d: %x != %x\n",
                    i, outData[i], inData[i]);
            errors++;
        }
    }
    if (errors) {
        printf("FAIL\n");
    } else {
        printf("PASS\n");
    }
    xdmaCloseChannel(&inChannel);
    xdmaCloseChannel(&outChannel);

    xdmaFreeKernelBuffer(inData, inHandle);
    xdmaFreeKernelBuffer(outData, outHandle);

    xdmaClose();

}
