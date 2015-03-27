#ifndef LIBXDMA_H
#define LIBXDMA_H

/*!
 * \file
 * libXDMA API
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>

#define FILEPATH "/dev/xdma"
#define MAP_SIZE  (33554432)
#define FILESIZE (MAP_SIZE * sizeof(uint8_t))


	enum xdma_wait {
		XDMA_WAIT_NONE = 0,
		XDMA_WAIT_SRC = (1 << 0),
		XDMA_WAIT_DST = (1 << 1),
		XDMA_WAIT_BOTH = (1 << 1) | (1 << 0),
	};

    //typedef int xdma_status;
    typedef enum {
        XDMA_SUCCESS = 0,
        XDMA_ERROR,
        XDMA_PENDING,
    }xdma_status;

    typedef enum {
        XDMA_TO_DEVICE = 0,
        XDMA_FROM_DEVICE = 1,
    } xdma_dir;

    typedef long unsigned int xdma_device;
    typedef long unsigned int xdma_channel;
    typedef long unsigned int xdma_transfer_handle;

    /*!
     * Initialize the DMA userspace library & userspace library
     */
    xdma_status xdmaOpen();

    /*!
     * Cleanup the userspace library & driver
     */
    xdma_status xdmaClose();

    /*!
     * Get the number of devices
     * \param[out] numDevices Number of devices
     */
    xdma_status xdmaGetNumDevices(int *numDevices);

    /*!
     * Get the device handles for the devices present in the system
     * \param entries[in]   Number of device handles that will be retrieved
     * \param devices[out]  Array that will hold the device handles.
     *      This should have enough capacity to hold at least entries elements
     * \param devs[out]     Number of handles copied to the devices array
     * \return XDMA_SUCCESS on success, XDMA_ERROR otherwise
     */
    xdma_status xdmaGetDevices(int entries, xdma_device *devices, int *devs);

    /*!
     * Open device channel
     * Each device can have 1 input + 1 output channel
     *
     * \param device[in]    Device that will be connected to the channel
     * \param directon[in]  Direction of the channel
     * \param flags[in]     Channel flags (TBD)
     * \param channel[out]  Handle to the recently open channel
     */
    xdma_status xdmaOpenChannel(xdma_device device, xdma_dir direction, unsigned int flags, xdma_channel *channel);
    /*!
     * Close a DMA channel and release its resources
     *
     * \param channel[in,out]   DMA channel which will be closed
     */
    xdma_status xdmaCloseChannel(xdma_channel *channel);


    /*!
     * Allocate a buffer in kernel space to be transferred to a xDMA device
     * \param buffer[out]   Pointer to the allocated buffer
     * \param len[in]       Buffer length in bytes
     */
    xdma_status xdmaAllocateKernelBuffer(void **buffer, size_t len);

    /*!
     * Free ALL kernel allocated buffers
     */
    xdma_status xdmaFreeKernelBuffers();

    /*!
     * Submit a kernel allocated buffer to be transferred through DMA
     * \param buffer[in]    Buffer to be transferred
     * \param len[in]       Buffer length
     * \param wait[in]      Wait until the transfer has finished {0,1}
     * \param channel[in]   DMA channel to operate
     * \param channel[out]  Pointer to the variable that will hold the transfer handle.
     *      If the transfer is blocking (wait == 1) the handle will not be valid. This pointer can be set to null
     */
    xdma_status xdmaSubmitKBuffer(void *buffer, size_t len, int wait, xdma_device dev, xdma_channel channel,
            xdma_transfer_handle *transfer);
    /*!
     * Wait or get the status of a dma transfer
     * \param transfer[in,out]  DMA transfer handle. If transfer has been completed,
     *      resources associated with this transfer will be freed
     * \param wait[in]          Wait until the transfer has been completed
     * \return                  XDMA_SUCCESS if the transfer has finished successfully
     *                          XDMA_PENDING if the pransfer has already not finished
     *                          XDMA_ERROR if an error has occured
     */
    xdma_status xdmaFinishTransfer(xdma_transfer_handle *transfer, int wait);



#ifdef __cplusplus
}
#endif
#endif				/* LIBXDMA_H */
