#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "libxdma.h"

#define DEFAULT_LEN     16
#define TEST_VAL        0xBADC0FEE

void usage(char *exe) {
    printf("Usage: %s <data length>", exe);
}

int main(int argc, char *argv[]) {
    int len;
    xdma_status status;
    if (argc < 2) {
        len = DEFAULT_LEN;
    } else if (!strcmp(argv[1], "--help")) {
        usage(argv[0]);
        return 0;
    } else {
        len = atoi(argv[1]);
    }
    //using 1 accelerator

    status = xdmaOpen();
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error initializing DMA\n");
        exit(1);
    }

    int *inData, *outData;
    //in =  malloc(len*sizeof(int));
    //out = malloc(len*sizeof(int));
    //Buffers must be allocated in kernel space mapped memory
    xdmaAllocateKernelBuffer((void **)&inData, len*sizeof(int));
    xdmaAllocateKernelBuffer((void **)&outData, len*sizeof(int));

    for (int i=0; i<len; i++) {
        inData[i] = TEST_VAL;
        outData[i] = 0;
    }

    xdma_device dev;
    status = xdmaGetDevices(1, &dev, NULL);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error getting platform devices\n");
        exit(1);
    }
    xdma_channel inChannel, outChannel;
    status = xdmaOpenChannel(dev, XDMA_TO_DEVICE, 0, &inChannel);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error opening input channel\n");
        exit(1);
    }
    status = xdmaOpenChannel(dev, XDMA_FROM_DEVICE, 0, &outChannel);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error opening output channel\n");
        exit(1);
    }

    xdma_transfer_handle inTransfer, outTransfer;
    status = xdmaSubmitKBuffer(inData, len*sizeof(int), 0, dev, inChannel, &inTransfer);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error submitting input transfer\n");
    }
    status = xdmaSubmitKBuffer(outData, len*sizeof(int), 0, dev, outChannel, &outTransfer);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error submitting output transfer\n");
    }
    status = xdmaFinishTransfer(&inTransfer, 1);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error during input transfer finalization");
    }
    status = xdmaFinishTransfer(&outTransfer, 1);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error during output transfer finalization");
    }

    //validate results
    int errors = 0;
    for (int i=0; i<len; i++) {
        if (outData[i] != inData[i]) {
            printf("Unexpected output at position %d: %c != %c\n",
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

    xdmaClose();

}
