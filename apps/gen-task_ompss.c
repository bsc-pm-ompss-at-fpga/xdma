#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define OUT_REF_VAL 0xDEADBEEF

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
    waited = nanos_fpga_alloc_dma_mem(sizeof(int));
    inData = nanos_fpga_alloc_dma_mem(inLen*sizeof(int));
    outData = nanos_fpga_alloc_dma_mem(outLen*sizeof(int));

    //set args
    args->in = inLen;
    args->wait = wait;
    args->out = outLen;

    //clear data
    memset(inData, 0, inLen*sizeof(int));
    memset(outData, 0, outLen*sizeof(int));
    *waited = 0;

    gen_task(inLen, outLen, args, inData, waited, outData);
#pragma omp taskwait

    //check data
    int errors = 0;
    if (*waited != wait) {
        errors++;
        fprintf(stderr, "Waited cycles do not match\n");
    }
    for (int i=0; i<outLen; i++) {
        if (outData[i] != OUT_REF_VAL) {
            errors++;
            fprintf(stderr, "Error in [%d] %d != %d\n", i, outData[i], OUT_REF_VAL);
        }
    }
    nanos_fpga_free_dma_mem();
    if (!errors) {
        printf("PASS\n");
        return 0;
    } else {
        printf("FAIL\n");
        printf("Program failed with %d errors\n", errors);
        return 1;
    }


}
