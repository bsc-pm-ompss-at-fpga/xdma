#include <stdio.h>

const int len = 10;

#pragma omp target device(fpga) copy_deps
#pragma omp task in([len]in) out([len]out)
void loopback_test(int *in, int *out) {
    for (int i=0; i<len; i++) {
        out[i] = in[i];
    }
}

#define     TEST_VAL   0xBADC0FEE
int main(int argc, char **argv) {
    int *input, *output;

    //allocate kernel space memory for dma transfers
    input = nanos_fpga_alloc_dma_mem(len*sizeof(int));
    output = nanos_fpga_alloc_dma_mem(len*sizeof(int));

    //Clear output buffer
    for (int i=0; i<len; i++) {
        output[i] = 0;
    }
    //Initialize data
    for (int i=0; i<len; i++) {
        input[i] = TEST_VAL;
    }

    loopback_test(input, output);
#pragma omp taskwait

    //validate results
    int errors = 0;
    for (int i=0; i<len; i++) {
        if (input[i] != output[i]) {
            fprintf(stderr, "Error in position %d: %d != %d\n",
                    i, input[i], output[i]);
            errors++;
        }
    }
    nanos_fpga_free_dma_mem();
    if (errors) {
        printf("FAIL\n");
        printf("Program failed with %d errors\n", errors);
    } else {
        printf("PASS\n");
    }
}
