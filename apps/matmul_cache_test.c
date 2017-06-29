#include <stdio.h>
#include <math.h>

#include "libxdma.h"

#define M_SIZE      32

void referenceMatmul(float *a, float *b, float *out_c) {
    for (int i=0; i<M_SIZE; i++) {
        for (int j=0; j<M_SIZE; j++){
            float sum=out_c[i*M_SIZE + j];
            for (int k=0; k<M_SIZE; k++) {
                sum += a[i*M_SIZE + k] * b[k *M_SIZE +j];
            }
            out_c[i*M_SIZE+j] = sum;
        }
    }
}
int main(int argc, char *argv[]){
    const float add = 0.0;
    const float factor = 2.0;
    //Init library
    xdmaOpen();
    xdma_device dev;
    //get 1st accelerator
    xdmaGetDevices(1, &dev, NULL);

    //Allocate matrices
    float *a, *b, *c, *cref;
    xdma_buf_handle aHandle, bHandle, cHandle;
    xdmaAllocateKernelBuffer((void*)&a, &aHandle, M_SIZE*M_SIZE*sizeof(float ));
    xdmaAllocateKernelBuffer((void*)&b, &bHandle, M_SIZE*M_SIZE*sizeof(float ));
    xdmaAllocateKernelBuffer((void*)&c, &cHandle, M_SIZE*M_SIZE*sizeof(float ));

    //Allocate reference matrix
    cref = malloc(M_SIZE*M_SIZE*sizeof(float));

    //Initialize matrices
    for (int i=0; i<M_SIZE; i++) {
        for (int j=0; j<M_SIZE; j++) {
            a[i*M_SIZE + j] = i*100 + j;
            c[i*M_SIZE + j] = add;
            b[i*M_SIZE + j] = 0.0;
            cref[i*M_SIZE + j] = add;
        }
    }
    for (int i=0; i<M_SIZE; i++) {
        b[i*M_SIZE + i] = factor;
    }


    xdma_task_handle task;
    //Initialize task
    xdmaInitTask(1, XDMA_COMPUTE_ENABLE, &task);

    //Set arguments
    xdmaAddArg(task, 0, XDMA_GLOBAL, aHandle, 0);
    xdmaAddArg(task, 1, XDMA_GLOBAL, bHandle, 0);
    xdmaAddArg(task, 2, XDMA_GLOBAL, cHandle, 0);

    //xdma_status xdmaSendTask(xdma_device dev, xdma_task_handle *taskHandle);
    xdmaSendTask(dev, task);

    //Wait for the task to finish
    xdmaWaitTask(task);

    xdma_instr_times *times;
    //Get hw instrumentation times
    xdmaGetInstrumentData(task, &times);

    //free the task
    xdmaDeleteTask(&task);

    //check results
    referenceMatmul(a, b, cref);
    float error = 0.0;
    for (int i=0; i<M_SIZE; i++) {
        for (int j=0; j<M_SIZE; j++) {
            float diff = fabs(c[i*M_SIZE + j] - cref[i*M_SIZE + j]);
            if (diff > 0.01 || (-diff < -0.1)) {
//                fprintf(stderr, "Error in element [%d][%d] = %f != %f\n",
//                        i, j, c[i*M_SIZE + j], cref[i*M_SIZE + j]);
            }
            error += diff;
        }
    }
    int exitStatus;
    if (error > 0.01) {
        printf("FAIL: Hardware results do not match reference values\n");
        exitStatus = 1;
    } else {
        printf("PASS\n");
        exitStatus = 0;
    }
    return exitStatus;

}
