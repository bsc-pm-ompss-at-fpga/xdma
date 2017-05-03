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
    xdmaGetDevices(1, &dev, NULL);
//    xdmaOpenChannel(dev, XDMA_TO_DEVICE, XDMA_CH_NONE, &inChannel);
//    xdmaOpenChannel(dev, XDMA_FROM_DEVICE, XDMA_CH_NONE, &outChannel);

    //Allocate matrices
    float *a, *b, *c, *cref;
    xdma_buf_handle aHandle, bHandle, cHandle;
    xdmaAllocateKernelBuffer((void*)&a, &aHandle, M_SIZE*M_SIZE*sizeof(float ));
    xdmaAllocateKernelBuffer((void*)&b, &bHandle, M_SIZE*M_SIZE*sizeof(float ));
    xdmaAllocateKernelBuffer((void*)&c, &cHandle, 2*M_SIZE*M_SIZE*sizeof(float ));

    //initialize matrices
    cref = malloc(M_SIZE*M_SIZE*sizeof(float));

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
    //Send task to the accelerator
    // xdma_status xdmaInitTask(int accId, int numInput, xdma_compute_flags compute,
    //         int numOutput, xdma_task_handle *taskDescriptor);



    //Force an offset in transfers
//    xdma_task_handle dummy_task;
//    xdmaInitTask(1, 3, XDMA_COMPUTE_ENABLE, 1, &dummy_task);
//
//
//    xdmaAddDataCopy(&dummy_task, 0, XDMA_GLOBAL, XDMA_TO_DEVICE, &aHandle,
//            M_SIZE*M_SIZE*sizeof(float), 0);
//    xdmaAddDataCopy(&dummy_task, 1, XDMA_GLOBAL, XDMA_TO_DEVICE, &bHandle,
//            M_SIZE*M_SIZE*sizeof(float), 0);
//    xdmaAddDataCopy(&dummy_task, 2, XDMA_GLOBAL, XDMA_TO_DEVICE, &cHandle,
//            M_SIZE*M_SIZE*sizeof(float), 0);
//
//    xdmaAddDataCopy(&dummy_task, 0, XDMA_GLOBAL, XDMA_FROM_DEVICE, &cHandle,
//            M_SIZE*M_SIZE*sizeof(float), M_SIZE*M_SIZE*sizeof(float));
//    xdmaSendTask(dev, &dummy_task);
//    xdmaWaitTask(dummy_task);

    xdma_task_handle task;
    xdmaInitTask(1, 3, XDMA_COMPUTE_ENABLE, 1, &task);

    //Set data copies
    //xdma_status xdmaAddDataCopy(xdma_task_handle *taskHandle,
    //        unsigned int paramId, xdma_mem_flags flags, xdma_dir direction,
    //        xdma_buf_handle *buffer, size_t size, unsigned int offset);
    xdmaAddDataCopy(&task, 0, XDMA_GLOBAL, XDMA_TO_DEVICE, &aHandle,
            M_SIZE*M_SIZE*sizeof(float), 0);
    xdmaAddDataCopy(&task, 1, XDMA_GLOBAL, XDMA_TO_DEVICE, &bHandle,
            M_SIZE*M_SIZE*sizeof(float), 0);
    xdmaAddDataCopy(&task, 2, XDMA_GLOBAL, XDMA_TO_DEVICE, &cHandle,
            M_SIZE*M_SIZE*sizeof(float), 0);

    xdmaAddDataCopy(&task, 0, XDMA_GLOBAL, XDMA_FROM_DEVICE, &cHandle,
            M_SIZE*M_SIZE*sizeof(float), 0);

    //xdma_status xdmaSendTask(xdma_device dev, xdma_task_handle *taskHandle);
    xdmaSendTask(dev, &task);

    //Wait for the task
    //xdma_status xdmaWaitTask(xdma_task_handle handle);
    xdmaWaitTask(task);

    xdma_instr_times *times;
    //xdma_status xdmaGetInstrumentData(xdma_task_handle task, xdma_instr_times **times);
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
