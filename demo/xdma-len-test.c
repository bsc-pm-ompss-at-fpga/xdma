// the below define is a hack
#define u32 unsigned int
#define dma_cookie_t unsigned int
#include "xdma.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#define FILEPATH "/dev/xdma"
#define MAP_SIZE  (4000)
#define FILESIZE (MAP_SIZE * sizeof(char))


int performTransfers(const int fd, const struct xdma_dev dev, const int LENGTH) {

    struct xdma_chan_cfg rx_config;
    rx_config.chan = dev.rx_chan;
    rx_config.dir = XDMA_DEV_TO_MEM;
    rx_config.coalesc = 1;
    rx_config.delay = 0;
    rx_config.reset = 0;
    if (ioctl(fd, XDMA_DEVICE_CONTROL, &rx_config) < 0) {
        perror("Error ioctl config rx chan");
        return -1;
    }

    struct xdma_chan_cfg tx_config;
    tx_config.chan = dev.tx_chan;
    tx_config.dir = XDMA_MEM_TO_DEV;
    tx_config.coalesc = 1;
    tx_config.delay = 0;
    tx_config.reset = 0;
    if (ioctl(fd, XDMA_DEVICE_CONTROL, &tx_config) < 0) {
        perror("Error ioctl config tx chan");
        return -1;
    }

    struct xdma_buf_info rx_buf;
    rx_buf.chan = dev.rx_chan;
    rx_buf.completion = dev.rx_cmp;
    rx_buf.cookie = (u32) NULL;
    rx_buf.buf_offset = (u32) 0;
    rx_buf.buf_size = (u32) LENGTH;
    rx_buf.dir = XDMA_DEV_TO_MEM;
    if (ioctl(fd, XDMA_PREP_BUF, &rx_buf) < 0) {
        perror("Error ioctl set rx buf");
        return -1;
    }

    struct xdma_buf_info tx_buf;
    tx_buf.chan = dev.tx_chan;
    tx_buf.completion = dev.tx_cmp;
    tx_buf.cookie = (u32) NULL;
    tx_buf.buf_offset = (u32) LENGTH;
    tx_buf.buf_size = (u32) LENGTH;
    tx_buf.dir = XDMA_MEM_TO_DEV;
    if (ioctl(fd, XDMA_PREP_BUF, &tx_buf) < 0) {
        perror("Error ioctl set tx buf");
        return -1;
    }

    struct xdma_transfer rx_trans;
    rx_trans.chan = dev.rx_chan;
    rx_trans.completion = dev.rx_cmp;
    rx_trans.cookie = rx_buf.cookie;
    rx_trans.wait = 0;
    if (ioctl(fd, XDMA_START_TRANSFER, &rx_trans) < 0) {
        perror("Error ioctl start rx trans");
        return -1;
    }

    struct xdma_transfer tx_trans;
    tx_trans.chan = dev.tx_chan;
    tx_trans.completion = dev.tx_cmp;
    tx_trans.cookie = tx_buf.cookie;
    tx_trans.wait = 0;
    if (ioctl(fd, XDMA_START_TRANSFER, &tx_trans) < 0) {
        perror("Error ioctl start tx trans");
        return -1;
    }
    return 0;
}

/*  Returns the number of devices
 *  -1 if error
 */
int getNumDevices(int fd) {
    int numDevices = 0;
    if (ioctl(fd, XDMA_GET_NUM_DEVICES, &numDevices) < 0) {
        perror("Error ioctl getting device num");
        return -1;
    }
    return numDevices;
}

int getDeviceInfo(int fd, int deviceId, struct xdma_dev *devInfo) {
    devInfo->tx_chan = (u32) NULL;
    devInfo->tx_cmp = (u32) NULL;
    devInfo->rx_chan = (u32) NULL;
    devInfo->rx_cmp = (u32) NULL;
    devInfo->device_id = deviceId;
    if (ioctl(fd, XDMA_GET_DEV_INFO, devInfo) < 0) {
        perror("Error ioctl getting device info");
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    int LENGTH, iter;
    if (argc > 2) {
        LENGTH = atoi(argv[1]);
        iter = atoi(argv[2]);
    } else {
        fprintf(stderr, "Usage: %s <num of bytes to transfer> <iterations>", argv[0]);
    }
    int i,j;
    int fd;
    char *map;		/* mmapped array of char's */
    char *result;
    int status = 0;

    result = (char*)malloc(LENGTH*sizeof(char));

    /* Open a file for writing.
     *  - Creating the file if it doesn't exist.
     *  - Truncating it to 0 size if it already exists. (not really needed)
     *
     * Note: "O_WRONLY" mode is not sufficient when mmaping.
     */
    fd = open(FILEPATH, O_RDWR | O_CREAT | O_TRUNC, (mode_t) 0600);
    if (fd == -1) {
        perror("Error opening file for writing");
        exit(EXIT_FAILURE);
    }

    /* mmap the file to get access to the memory area.
     * Mmap kernel memory into user space
     */
    map = mmap(0, 2*LENGTH, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        perror("Error mmapping the file");
        exit(EXIT_FAILURE);
    }

    for (j=0; j<iter; j++) {
        /* Now write int's to the file as if it were memory (an array of ints).
        */
        // fill tx with a value
        for (i = 0; i < LENGTH; i++) {
            map[LENGTH + i] = 'D';
        }

        // fill rx with a value
        for (i = 0; i < LENGTH; i++) {
            map[i] = 'C';
        }

        struct xdma_dev dev;
        int ndevs;
        ndevs = getNumDevices(fd);
        getDeviceInfo(fd, ndevs-1, &dev);
        performTransfers(fd, dev, LENGTH);

        //Validate results

        int errors = 0;
        usleep(5000); //5ms
        result = map;
        for (i=0; i<LENGTH; i++) {
            if (result[i] != 'D') { //rx buffer
                if (errors < 10)
                    fprintf(stderr, "[%d]: %c\n", i, result[i]);
                errors++;
            }
        }
        if (errors) {
            fprintf(stderr, "Iteration %d failed with %d errors\n", j, errors);
            status = -1;
        }

    }

    /* Don't forget to free the mmapped memory
    */
    if (munmap(map, FILESIZE) == -1) {
        perror("Error un-mmapping the file");
        /* Decide here whether to close(fd) and exit() or not. Depends... */
    }
    /* Un-mmaping doesn't close the file, so we still need to do that.  */
    close(fd);

    if (status) {
        printf("FAIL\n");
    } else {
        printf("PASS\n");
    }

    return status;
}
