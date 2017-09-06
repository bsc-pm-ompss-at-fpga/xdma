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
#define INSTR_FILEPATH  "/dev/xdma_instr"
#define MAP_SIZE  (33554432)
#define FILESIZE (MAP_SIZE * sizeof(uint8_t))


	enum xdma_wait {
		XDMA_WAIT_NONE = 0,
		XDMA_WAIT_SRC = (1 << 0),
		XDMA_WAIT_DST = (1 << 1),
		XDMA_WAIT_BOTH = (1 << 1) | (1 << 0),
	};

    //TODO: Proper error codes
    /**
     * xdma status
     */
    typedef enum {
        XDMA_SUCCESS = 0,   ///< Operation finished sucessfully
        XDMA_ERROR,         ///< Operation finished with an error
        XDMA_PENDING,       ///< Operation not yet finished
        XDMA_EINVAL,        ///< Invalid operation arguments
        XDMA_ENOMEM,        ///< Operation failed due to an error allocating memory
        XDMA_EACCES,        ///< Operation failed because user does not have access
        XDMA_ENOENT,        ///< Operation failed because device does not exist
    } xdma_status;

    /// Channel direcction
    typedef enum {
        XDMA_TO_DEVICE = 0,     ///< From host main memory to device
        XDMA_FROM_DEVICE = 1,   ///< From device to host main memory
    } xdma_dir;

    /// Transfer mode for (non)blocking operation
    typedef enum {
        XDMA_ASYNC = 0,     ///< Asynchronous transfer (non blocking)
        XDMA_SYNC = 1,      ///< Synchronous transfer (blocking)
    } xdma_xfer_mode;

    typedef long unsigned int xdma_device;
    typedef long unsigned int xdma_channel;
    typedef long unsigned int xdma_transfer_handle;
    typedef void* xdma_buf_handle;

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
     * \param[in] entries   Number of device handles that will be retrieved
     * \param[out] devices  Array that will hold the device handles.
     *      This should have enough capacity to hold at least entries elements
     * \param[out] devs     Number of handles copied to the devices array
     * \return XDMA_SUCCESS on success, XDMA_ERROR otherwise
     */
    xdma_status xdmaGetDevices(int entries, xdma_device *devices, int *devs);

    /*!
     * Get the device channel handle
     * Each device can have 1 input + 1 output channel
     *
     * \param[in] device     Device that will be connected to the channel
     * \param[in] direction  Direction of the channel
     * \param[out] channel   Handle to the channel
     */
    xdma_status xdmaGetDeviceChannel(xdma_device device, xdma_dir direction, xdma_channel *channel);

    /*!
     * Allocate a buffer in kernel space to be transferred to a xDMA device
     * \param[out] buffer   Pointer to the allocated buffer
     * \param[out] handle   DMA buffer handle
     * \param[in] len       Buffer length in bytes
     */
    xdma_status xdmaAllocateKernelBuffer(void **buffer, xdma_buf_handle *handle, size_t len);

    /*!
     * Free a pinned buffer allocated in kernel space and unmap the region from user space
     * \param[in] buffer    Address of the bointer to bee freed
     * \param[in] handle    Buffer handle to be freed
     */
    xdma_status xdmaFreeKernelBuffer(void *buffer, xdma_buf_handle handle);

    /*!
     * Submit a pinned buffer allocated in kernel space
     * \param[in] buffer    Buffer handle
     * \param[in] len       Buffer length
     * \param[in] offset    Transfer offset
     * \param[in] mode      Transfer mode. Either XDMA_SYNC or XDMA_ASYNC
     *                      for sync (blocking) or async (non blocking) transfers
     * \param[in] dev       DMA device to transfer data
     * \param[in] channel   DMA channel to operate
     * \param[out] transfer Pointer to the variable that will hold the transfer handle.
     *      If the transfer is blocking (XDMA_SUCCESS), this pointer should be NULL
     * \return              XDMA_SUCCESS on success, XDMA_ERROR otherwise
     */
    xdma_status xdmaSubmitKBuffer(xdma_buf_handle buffer, size_t len, unsigned int offset,
            xdma_xfer_mode mode, xdma_device dev, xdma_channel channel,
            xdma_transfer_handle *transfer);

    /*!
     * Submit a user allocated buffer (i.e. using malloc) to be transferred through DMA
     * \param[in] buffer    Buffer to be transferred
     * \param[in] len       Buffer length
     * \param[in] mode      Transfer mode. Either XDMA_SYNC or XDMA_ASYNC
     *                      for sync (blocking) or async (non blocking) transfers
     * \param[in] dev       DMA device to transfer data
     * \param[in] channel   DMA channel to operate
     * \param[out] transfer Pointer to the variable that will hold the transfer handle.
     *      If the transfer is blocking (XDMA_SUCCESS), this pointer should be NULL
     */
    xdma_status xdmaSubmitBuffer(void *buffer, size_t len, xdma_xfer_mode mode,
            xdma_device dev, xdma_channel channel, xdma_transfer_handle *transfer);
    /*!
     * Test the status of a transfer (finished, pending or error)
     * \param[in] transfer  DMA transfer handle to be tested
     * \return              Transfer status
     *                      XDMA_SUCCESS if the transfer has finish successfully
     *                      XDMA_PENDING if the transfer has not yet finished
     *                      XDMA_ERROR if an error has occurred
     */
    xdma_status xdmaTestTransfer(xdma_transfer_handle transfer);

    /*!
     * Wait for a transfer to finish
     * \param[in] transfer  DMA transfer handle
     * \return              XDMA_SUCCESS if the transfer has finished successfully
     *                      XDMA_PENDING if the transfer has already not finished
     *                      XDMA_ERROR if an error has occurred
     */
    xdma_status xdmaWaitTransfer(xdma_transfer_handle transfer);

    /*!
     * Release the data structures associated with a DMA transfer
     * \param[in,out] transfer  DMA transfer handle to be released
     * \return                  XDMA_SUCCESS if transfer successfully released
     *                          XDMA_ERROR otherwise
     */
    xdma_status xdmaReleaseTransfer(xdma_transfer_handle *transfer);

    xdma_status xdmaGetDMAAddress(xdma_buf_handle buffer, unsigned long *dmaAddress);

    /*!
     * Initialize the support for HW instrumentation.
     * Note that the function will fail if the HW instrumentation support is not available in the loaded
     * bitstream.
     * \return  XDMA_SUCCESS  if the support is successfully initialized
     *          XDMA_EISINIT  if the support is already initialized
     *          XDMA_ERROR    otherwise
     */
    xdma_status xdmaInitHWInstrumentation();

    /*!
     * Finalize the support for HW instrumentation
     * \return  XDMA_SUCCESS  if the support is successfully finalized
     *          XDMA_ERROR    otherwise
     */
    xdma_status xdmaFiniHWInstrumentation();

    xdma_status xdmaGetDeviceTime(uint64_t *time);
    int xdmaInstrumentationEnabled();
    unsigned long xdmaGetInstrumentationTimerAddr();

#ifdef __cplusplus
}
#endif
#endif				/* LIBXDMA_H */
