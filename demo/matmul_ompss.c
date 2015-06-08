#include <math.h>
#include <stdio.h>
#include <stdlib.h>

const int M_SIZE = 64;

#pragma omp target device(fpga) copy_deps
#pragma omp task in([M_SIZE*M_SIZE]a, [M_SIZE*M_SIZE]b) inout([M_SIZE*M_SIZE]c)
void matmul(float *a, float *b, float *c) {
    for (int i=0; i<M_SIZE; i++) {
        for (int j=0; j<M_SIZE; j++){
            for (int k=0; k<M_SIZE; k++) {
                c[i*M_SIZE + j] += a[i*M_SIZE + k] * b[k *M_SIZE +j];
            }
        }
    }
}

void ref_matmul(float *a, float *b, float *out_c) {
    for (int i=0; i<M_SIZE; i++) {
        for (int j=0; j<M_SIZE; j++){
            for (int k=0; k<M_SIZE; k++) {
                out_c[i*M_SIZE + j] += a[i*M_SIZE + k] * b[k *M_SIZE +j];
            }
        }
    }
}

int main(int argc, char *argv[]) {

    float *a, *b, *c, *ref;
    a = nanos_fpga_alloc_dma_mem(M_SIZE*M_SIZE*sizeof(float));
    b = nanos_fpga_alloc_dma_mem(M_SIZE*M_SIZE*sizeof(float));
    c = nanos_fpga_alloc_dma_mem(M_SIZE*M_SIZE*sizeof(float));
    ref = malloc(M_SIZE*M_SIZE*sizeof(float));

    if (!a || !b || !c || !ref) {
        free(ref);
        nanos_fpga_free_dma_mem();
        fprintf(stderr, "Error allocating memory\n");
        return 1;
    }

    for (int i=0; i<M_SIZE; i++) {
        for (int j=0; j<M_SIZE; j++) {
            a[i*M_SIZE + j] = i*100 + j;
            b[i*M_SIZE + j] = i;
            c[i*M_SIZE + j] = 0;
            ref[i*M_SIZE + j] = 0;
        }
    }

    ref_matmul(a, b, ref); //TODO: make this a task
    matmul(a, b, c);
#pragma omp taskwait

    float error = 0.0;
    for (int i=0; i<M_SIZE; i++) {
        for (int j=0; j<M_SIZE; j++) {
            float diff = fabsf(c[i*M_SIZE + j] - ref[i*M_SIZE + j]);
            if (diff > 0.01) {
                fprintf(stderr, "Error in element [%d][%d] = %f != %f\n",
                        i, j, c[i*M_SIZE + j], ref[i*M_SIZE + j]);
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

    nanos_fpga_free_dma_mem();
    free(ref);

}