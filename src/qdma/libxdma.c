#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


#include "../libxdma.h"

//#include "qdma_nl.h"
#include "qdmautils.h"
#include "qdma_nl.h"

#define QDMA_DEV_ID 0x02000
#define QDMA_Q_IDX  1

int _qdmaFd;



static void parse_dev_list(const char *devList) {
    //parse device list
    //printf("%s\n", devList);
}


xdma_status xdmaOpen() {

    //get device info in order to know which files we need to open
    struct xcmd_info cmd;
    memset(&cmd, 0, sizeof(struct xcmd_info));
    //cmd.op = XNL_CMD_DEV_LIST;
    //cmd.log_msg_dump = parse_dev_list;

    //qdma_dev_list_dump(&cmd); //dumps a list of devices in a string

    //FIXME assuming single qdma device
    //Create queue
    //  Creating single queue
    //Assuming that the device has already a maximum number of queues defined
    cmd.if_bdf = QDMA_DEV_ID;

    //add a queue
    cmd.op = XNL_CMD_Q_ADD;
    cmd.req.qparm.sflags = 1 << QPARM_MODE;
    cmd.req.qparm.flags |= XNL_F_QMODE_MM;
    cmd.req.qparm.sflags |= 1 << QPARM_IDX;
    cmd.req.qparm.idx = QDMA_Q_IDX;
    cmd.req.qparm.num_q = 1;
    cmd.req.qparm.sflags |= 1 << QPARM_DIR;
    cmd.req.qparm.flags |= XNL_F_QDIR_BOTH;
    qdma_q_add(&cmd);

    cmd.op = XNL_CMD_Q_START;   //Reuse parameters
    //start queue
    qdma_q_start(&cmd);

    //Open files
    char devFileName[24];
    sprintf(devFileName, "/dev/qdma%05x-MM-%d", QDMA_DEV_ID, QDMA_Q_IDX);
    printf("%s\n", devFileName);

    _qdmaFd = open(devFileName, O_RDWR);
    if (_qdmaFd < 0) {
        perror("XDMA: ");
        return XDMA_ERROR;
    }
    //TODO: Cleanup in case of error

}

/*!
 * Cleanup the userspace library & driver
 */
xdma_status xdmaClose() {

    //close queue files
    //stop queues
    //delete queues
}

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
 * Allocate a buffer to be transferred to a xDMA device and accessible from host and device
 * \param[out] buffer   Pointer to the allocated buffer
 * \param[out] handle   Buffer handle
 * \param[in] len       Buffer length in bytes
 */
xdma_status xdmaAllocateHost(void **buffer, xdma_buf_handle *handle, size_t len);

/*!
 * Allocate a buffer to be transferred to a xDMA device (not accessible from the host)
 * \param[out] handle   Buffer handle
 * \param[in] len       Buffer length in bytes
 */
xdma_status xdmaAllocate(xdma_buf_handle *handle, size_t len);

/*!
 * Free a buffer
 * \param[in] handle    Buffer handle to be freed (may be from any xdmaAllocate)
 */
xdma_status xdmaFree(xdma_buf_handle handle);

/*!
 * Submit a buffer in a device channel
 * \param[in] buffer    Buffer handle
 * \param[in] len       Buffer length
 * \param[in] offset    Transfer offset
 * \param[in] dev       DMA device to transfer data
 * \param[in] channel   DMA channel to operate
 * \param[out] transfer Pointer to the variable that will hold the transfer handle.
 *                      (Only available in async version)
 * NOTE: An async operation must be synchronized at some point using xdmaTestTransfer
 *       or xdmaWaitTransfer. Otherwise, the execution may have memory leaks or even
 *       hang
 */
xdma_status xdmaStream(xdma_buf_handle buffer, size_t len, unsigned int offset,
        xdma_device dev, xdma_channel channel);
xdma_status xdmaStreamAsync(xdma_buf_handle buffer, size_t len, unsigned int offset,
        xdma_device dev, xdma_channel channel, xdma_transfer_handle *transfer);

/*!
 * Copy to/from a buffer from/to userspace memory
 *
 * \param[in] usr       Pointer to userspace memory
 * \param[in] buffer    Buffer handle
 * \param[in] len       Length of the data movement (in bytes)
 * \param[in] offset    Offset to apply in the buffer
 * \param[in] mode      Directionality of the data movement
 *                       - XDMA_TO_DEVICE:   buffer[offset .. offset+len] = usr[0 .. len]
 *                       - XDMA_FROM_DEVICE: usr[0 .. len] = buffer[offset .. offset+len]
 * \param[out] transfer Pointer to the variable that will hold the transfer handle.
 *                      (Only available in async version)
 * NOTE: An async operation must be synchronized at some point using xdmaTestTransfer
 *       or xdmaWaitTransfer. Otherwise, the execution may have memory leaks or even
 *       hang
 */
xdma_status xdmaMemcpy(void *usr, xdma_buf_handle buffer, size_t len, unsigned int offset,
        xdma_dir mode);
xdma_status xdmaMemcpyAsync(void *usr, xdma_buf_handle buffer, size_t len, unsigned int offset,
        xdma_dir mode, xdma_transfer_handle *transfer);

/*!
 * Test the status of a transfer (finished, pending or error)
 * \param[in] transfer  DMA transfer handle to be tested
 * \return              Transfer status
 *                      XDMA_SUCCESS if the transfer has finish successfully
 *                      XDMA_PENDING if the transfer has not yet finished
 *                      XDMA_ERROR if an error has occurred
 */
xdma_status xdmaTestTransfer(xdma_transfer_handle *transfer);

/*!
 * Wait for a transfer to finish
 * \param[in] transfer  Transfer handle
 * \return              XDMA_SUCCESS if the transfer has finished successfully
 *                      XDMA_PENDING if the transfer has already not finished
 *                      XDMA_ERROR if an error has occurred
 */
xdma_status xdmaWaitTransfer(xdma_transfer_handle *transfer);

/*!
 * Get the internal device address to reference a buffer
 * \param[in] buffer      Buffer handle
 * \param[out] devAddress Address of buffer in the device
 * \return                XDMA_SUCCESS if the devAddress has been successfully set,
 *                        XDMA_ERROR otherwise
 */
xdma_status xdmaGetDeviceAddress(xdma_buf_handle buffer, unsigned long *devAddress);

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

/*!
 * Get the internal device time (instrumentation timer)
 * \param[out] time Current timestamp in the device
 * \return          XDMA_SUCCESS if the time has been successfully set,
 *                  XDMA_ERROR otherwise
 */
xdma_status xdmaGetDeviceTime(uint64_t *time);

/*!
 * Check if the instrumentation is initialized and available
 * \return 1 if the instrumentation support has been sucessfully initialized,
 *         0 otherwise
 */
int xdmaInstrumentationEnabled();

/*!
 * Get the internal device address of the instrumentation timer
 * \return Address of the instrumentation times in the device,
 *         0 if any error happened
 */
uint64_t xdmaGetInstrumentationTimerAddr();

/*!
 * Initialize DMA memory subsystem
 * Any call to memory related functions such as xdmaAllocate
 * prior to memory initialization will fail.
 * \return  XDMA_SUCCESS    if memory subsystem was successfully initialized
 *          XDMA_EACCES     if user does not have permission to access memory node
 *          XDMA_ENOENT     if memory node does not exist
 *          XDMA_ERROR      in case of any other error
 */
xdma_status xdmaInitMem();

/*!
 * Deinitialize the DMA memory subsystem
 * \return  XDMA_SUCCESS    on success
 *          XDMA_ERROR      otherwise
 */
xdma_status xdmaFiniMem();

