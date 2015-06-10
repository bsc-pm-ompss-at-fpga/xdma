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

void printMatrix(int nElem, float *mat) {
    for (int i=0; i<nElem; i++) {
        for (int j=0; j<nElem; j++) {
            printf("%.1f ", mat[i*M_SIZE + j]);
        }
        printf("\n");
    }
}

static inline void checkError(xdma_status status, const char * msg) {
    if (status != XDMA_SUCCESS) {
        fprintf(stderr, msg);
    }
}

int main(int argc, char *argv[]) {


    float factor = 2.0;
    float add = 0.0;

    if (argc == 1) { //No argument given
        fprintf(stderr, "Usage: %s <factor> <add>\n"
                "defaults to 2.0, 0.1\n", argv[0]);
    } else {
        factor = atof(argv[1]);
        if (argc > 2) {
            add = atof(argv[2]);
        }
    }

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
            c[i*M_SIZE + j] = add;
            result[i*M_SIZE + j] = 0.0;
            cref[i*M_SIZE + j] = add;
        }
    }
    //initialize diagonal matrix
    for (int i=0; i<M_SIZE; i++) {
        b[i*M_SIZE + i] = factor;
    }

    xdma_device dev;
    status = xdmaGetDevices(1, &dev, NULL);
    checkError(status, "Error getting platform devices\n");

    xdma_channel inChannel, outChannel;
    status = xdmaOpenChannel(dev, XDMA_TO_DEVICE, 0, &inChannel);
    checkError(status, "Error opening input channel\n");
    status = xdmaOpenChannel(dev, XDMA_FROM_DEVICE, 0, &outChannel);
    checkError(status, "Error opening output channel\n");

    //transfer data
    xdma_transfer_handle aTrans, bTrans, cTrans, outTrans;
    status = xdmaSubmitKBuffer(a, M_SIZE*M_SIZE*sizeof(float), 0, dev, inChannel, &aTrans);
        checkError(status, "Error submitting A matrix\n");
    status = xdmaSubmitKBuffer(b, M_SIZE*M_SIZE*sizeof(float), 0, dev, inChannel, &bTrans);
        checkError(status, "Error submitting B matrix\n");
    status = xdmaSubmitKBuffer(c, M_SIZE*M_SIZE*sizeof(float), 0, dev, inChannel, &cTrans);
        checkError(status, "Error submitting C matrix\n");
    status = xdmaSubmitKBuffer(result, M_SIZE*M_SIZE*sizeof(float), 0, dev, outChannel, &outTrans);
        checkError(status, "Error submitting result matrix\n");

    status = xdmaWaitTransfer(aTrans);
    checkError(status, "Error waiting for transfer A\n");
    status = xdmaWaitTransfer(bTrans);
    checkError(status, "Error waiting for transfer B\n");
    status = xdmaWaitTransfer(cTrans);
    checkError(status, "Error waiting for transfer C\n");
    status = xdmaWaitTransfer(outTrans);
    checkError(status, "Error waiting for output transfer\n");

    xdmaReleaseTransfer(&aTrans);
    xdmaReleaseTransfer(&bTrans);
    xdmaReleaseTransfer(&cTrans);
    xdmaReleaseTransfer(&outTrans);

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
    printf("A matrix:\n");
    printMatrix(10, a);
    printf("Result matrix (a*b + c)\n");
    printMatrix(10, result);

    int exitStatus;
    if (error > 0.01) {
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
