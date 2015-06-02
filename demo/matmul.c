#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include <unistd.h>

#include "libxdma.h"

#define M_SIZE 64

//Reference matrix multiplication for validation purposes
void referenceMatmul(float *a, float *b, float *out_c) {
    for (int i=0; i<M_SIZE; i++) {
        for (int j=0; j<M_SIZE; j++){
            for (int k=0; k<M_SIZE; k++) {
                out_c[i*M_SIZE + j] += a[i*M_SIZE + k] * b[k *M_SIZE +j];
            }
        }
    }
}

static inline void checkError(xdma_status status, char * msg) {
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, msg);
    }
}

int main(int argc, char *argv[]) {
    float *a, *b, *c, *cref, *result;
    xdma_status status;

    status = xdmaOpen();
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error initializing DMA\n");
        exit(1);
    }

    xdmaAllocateKernelBuffer((void*)&a, M_SIZE*M_SIZE*sizeof(float));
    xdmaAllocateKernelBuffer((void*)&b, M_SIZE*M_SIZE*sizeof(float));
    xdmaAllocateKernelBuffer((void*)&c, M_SIZE*M_SIZE*sizeof(float));
    xdmaAllocateKernelBuffer((void*)&result, M_SIZE*M_SIZE*sizeof(float));

    cref = malloc(M_SIZE*M_SIZE*sizeof(float));

    for (int i=0; i<M_SIZE; i++) {
        for (int j=0; j<M_SIZE; j++) {
            a[i*M_SIZE + j] = i*100 + j;
            b[i*M_SIZE + j] = i;
            c[i*M_SIZE + j] = 0;
            result[i*M_SIZE + j] = 0.0;
            cref[i*M_SIZE + j] = 0;
        }
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

    //transfer data
    xdma_transfer_handle aTrans, bTrans, cTrans, outTrans;
    status = xdmaSubmitKBuffer(a, M_SIZE*M_SIZE*sizeof(float), 0, dev, inChannel, &aTrans);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error submitting A matrix\n");
    }
    status = xdmaSubmitKBuffer(b, M_SIZE*M_SIZE*sizeof(float), 0, dev, inChannel, &bTrans);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error submitting B matrix\n");
    }
    status = xdmaSubmitKBuffer(c, M_SIZE*M_SIZE*sizeof(float), 0, dev, inChannel, &cTrans);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error submitting C matrix\n");
    }
    status = xdmaSubmitKBuffer(result, M_SIZE*M_SIZE*sizeof(float), 0, dev, outChannel, &outTrans);
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, "Error submitting result matrix\n");
    }

    status = xdmaFinishTransfer(&aTrans, 1);
    checkError(status, "Error waiting for transfer A\n");
    status = xdmaFinishTransfer(&bTrans, 1);
    checkError(status, "Error waiting for transfer B\n");
    status = xdmaFinishTransfer(&cTrans, 1);
    checkError(status, "Error waiting for transfer C\n");
    status = xdmaFinishTransfer(&outTrans, 1);
    checkError(status, "Error waiting for output transfer\n");

    //run reference matmul & validate
    referenceMatmul(a, b, cref);

    float error = 0.0;
    for (int i=0; i<M_SIZE; i++) {
        for (int j=0; j<M_SIZE; j++) {
            float diff = fabsf(result[i*M_SIZE + j] - cref[i*M_SIZE + j]);
            if (diff > 0.01) {
                fprintf(stderr, "Error in element [%d][%d] = %f != %f\n",
                        i, j, result[i*M_SIZE + j], cref[i*M_SIZE + j]);
            }
            error += diff;
        }
    }
    int exitStatus;
    if (error > 0.01) { //TODO: set a more intelligent threshold
        printf("FAIL: Hardware results do not match reference values\n");
        exitStatus = 1;
    } else {
        printf("PASS\n");
        exitStatus = 0;
    }

    //shutdown

    free(cref);
    xdmaCloseChannel(&inChannel);
    xdmaCloseChannel(&outChannel);

    xdmaClose();

    return exitStatus;
}
