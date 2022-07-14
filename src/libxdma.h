/*--------------------------------------------------------------------
  (C) Copyright 2017-2020 Barcelona Supercomputing Center
                          Centro Nacional de Supercomputacion

  This file is part of OmpSs@FPGA toolchain.

  This code is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 3 of
  the License, or (at your option) any later version.

  OmpSs@FPGA toolchain is distributed in the hope that it will be
  useful, but WITHOUT ANY WARRANTY; without even the implied
  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this code. If not, see <www.gnu.org/licenses/>.
--------------------------------------------------------------------*/

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
    XDMA_ENOSYS,        ///< Function not implemented
} xdma_status;

/// Channel direcction
typedef enum {
    XDMA_TO_DEVICE = 0,         ///< From host to device
    XDMA_FROM_DEVICE = 1,       ///< From device to host
    XDMA_DEVICE_TO_DEVICE = 2   ///< From device to device
} xdma_dir;

typedef long unsigned int xdma_device;
typedef long unsigned int xdma_channel;
typedef long unsigned int xdma_transfer_handle;
typedef void* xdma_buf_handle;

/*!
 * Initialize the DMA userspace library & userspace library
 */
xdma_status xdmaInit();

/*!
 * Cleanup the userspace library & driver
 */
xdma_status xdmaFini();

/*!
 * Get the number of devices
 * \param[out] numDevices Number of devices
 */
xdma_status xdmaGetNumDevices(int *numDevices);

/*!
 * Allocate a buffer to be transferred to an xDMA device and accessible from host and device
 * \param[in] devId     Device id
 * \param[out] buffer   Pointer to the allocated buffer
 * \param[out] handle   Buffer handle
 * \param[in] len       Buffer length in bytes
 */
xdma_status xdmaAllocateHost(int devId, void **buffer, xdma_buf_handle *handle, size_t len);

/*!
 * Allocate a buffer to be transferred to an xDMA device (not accessible from the host)
 * \param[in] devId     Device id
 * \param[out] handle   Buffer handle
 * \param[in] len       Buffer length in bytes
 */
xdma_status xdmaAllocate(int devId, xdma_buf_handle *handle, size_t len);

/*!
 * Free a buffer
 * \param[in] handle    Buffer handle to be freed (may be from any xdmaAllocate)
 */
xdma_status xdmaFree(xdma_buf_handle handle);

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
xdma_status xdmaMemcpy(void *usr, xdma_buf_handle buffer, size_t len, size_t offset,
                       xdma_dir mode);
xdma_status xdmaMemcpyAsync(void *usr, xdma_buf_handle buffer, size_t len, size_t offset,
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
 * \param[in] devId Device id
 * \param[out] time Current timestamp in the device
 * \return          XDMA_SUCCESS if the time has been successfully set,
 *                  XDMA_ERROR otherwise
 */
xdma_status xdmaGetDeviceTime(int devId, uint64_t *time);

/*!
 * Check if the instrumentation is initialized and available
 * \return 1 if the instrumentation support has been sucessfully initialized,
 *         0 otherwise
 */
int xdmaInstrumentationEnabled();

/*!
 * Get the internal device address of the instrumentation timer
 * \param[in] Device id
 * \return Address of the instrumentation times in the device,
 *         0 if any error happened
 */
uint64_t xdmaGetInstrumentationTimerAddr(int devId);

#ifdef __cplusplus
}
#endif
#endif /* LIBXDMA_H */
