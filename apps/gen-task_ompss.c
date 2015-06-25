#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "timing.h"

const int IN_MAGIC_VAL = 0xC0FFEE;
const int OUT_REF_VAL  = 0xDEADBEEF;

typedef struct {
    int in;
    int wait;
    int out;
}args_t ;
//
#pragma omp target device(fpga) copy_deps
#pragma omp task in([1]args, [_inl]inData) out([1]waited, [_outl]outData)
void gen_task(int _inl, int _outl, args_t *args, int *inData, int *waited, int *outData) {
    int inLen, outLen, wait;
    inLen = args->in;
    wait = args->wait;
    outLen = args->out;
    waited = 0;

    for (int i=0; i<*waited; i++) {
        waited++;
    }

    for (int i=0; i<outLen; i++) {
        outData[i] = OUT_REF_VAL;
    }
}

int main(int argc, char *argv[]) {
    int inLen, wait, outLen, iter;
    if (argc > 4) {
        inLen = atoi(argv[1]);
        wait = atoi(argv[2]);
        outLen = atoi(argv[3]);
        iter = atoi(argv[4]);
    } else {
        fprintf(stderr, "usage: %s <in_data> <wait_cycles> <out_data> <iteration>\n"
                "Note that data length is specified as 32-bit elements\n", argv[0]);
        return 1;
    }
    int *inData, *outData;
    int *waited;
    args_t *args;

    args = nanos_fpga_alloc_dma_mem(sizeof(args_t));
    waited = nanos_fpga_alloc_dma_mem(sizeof(int)*iter);
    inData = nanos_fpga_alloc_dma_mem(inLen*sizeof(int)*iter);
    outData = nanos_fpga_alloc_dma_mem(outLen*sizeof(int)*iter);

    //set args
    args->in = inLen;
    args->wait = wait;
    args->out = outLen;

    //clear data
    memset(outData, 0, outLen*sizeof(int)*iter);
    memset(waited, 0, sizeof(int)*iter);
    for (int i=0; i<inLen*iter; i++) {
        inData[i] = IN_MAGIC_VAL;
    }

    START_COUNT_TIME;
    for (int i=0; i<iter; i++) {
        gen_task(inLen, outLen, args, &inData[inLen*i], &waited[i], &outData[outLen*i]);
    }
#pragma omp taskwait
    STOP_COUNT_TIME("");

    //check data
    int errors = 0;


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

    nanos_fpga_free_dma_mem();
    if (errors) {
        printf("FAIL\n");
        printf("Program failed with %d errors\n", errors);
        return 1;
    }
    return 0;


}
