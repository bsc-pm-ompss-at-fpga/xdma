#ifndef XDMA_H
#define XDMA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <linux/types.h>
#include <asm/ioctl.h>

#define MODULE_NAME	"xdma"
#define DMA_LENGTH	(32*1024*1024)
#define MAX_DEVICES     4

#define XDMA_IOCTL_BASE	'W'
#define XDMA_GET_NUM_DEVICES	_IO(XDMA_IOCTL_BASE, 0)
#define XDMA_GET_DEV_INFO	_IO(XDMA_IOCTL_BASE, 1)
#define XDMA_DEVICE_CONTROL	_IO(XDMA_IOCTL_BASE, 2)
#define XDMA_PREP_BUF		_IO(XDMA_IOCTL_BASE, 3)
#define XDMA_START_TRANSFER	_IO(XDMA_IOCTL_BASE, 4)
#define XDMA_STOP_TRANSFER	_IO(XDMA_IOCTL_BASE, 5)
/*#define XDMA_TEST_TRANSFER	_IO(XDMA_IOCTL_BASE, 6) Unused */
#define XDMA_FINISH_TRANSFER _IO(XDMA_IOCTL_BASE, 7)
#define XDMA_PREP_USR_BUF	_IO(XDMA_IOCTL_BASE, 8)
#define XDMA_RELEASE_USR_BUF	_IO(XDMA_IOCTL_BASE, 9)
#define XDMA_GET_LAST_KBUF	_IO(XDMA_IOCTL_BASE, 10)
#define XDMA_RELEASE_KBUF	_IO(XDMA_IOCTL_BASE, 11)
#define XDMA_GET_DMA_ADDRESS	_IO(XDMA_IOCTL_BASE, 12)


	enum xdma_direction {
		XDMA_MEM_TO_DEV,
		XDMA_DEV_TO_MEM,
		XDMA_TRANS_NONE,
	};

	enum xdma_transfer_status {
		XDMA_DMA_TRANSFER_FINISHED = 0,
		XDMA_DMA_TRANSFER_PENDING,
	};

	struct xdma_dev {
		struct dma_chan *tx_chan;	/* (struct dma_chan *) */
		struct completion *tx_cmp;	/* (struct completion *) callback_param */
		struct dma_chan *rx_chan;	/* (struct dma_chan *) */
		struct completion *rx_cmp;	/* (struct completion *) callback_param */
		u32 device_id;
	};

	struct xdma_chan_cfg {
		struct dma_chan *chan;	/* (struct dma_chan *) */

		enum xdma_direction dir;	/* Channel direction */
		int coalesc;	/* Interrupt coalescing threshold */
		int delay;	/* Delay counter */
		int reset;	/* Reset Channel */
	};

	struct xdma_buf_info {
		struct dma_chan *chan;	/* (struct dma_chan *) */
		struct completion *completion;	/* (struct completion *) callback_param */

		dma_cookie_t cookie;
		unsigned long address;
		u32 buf_offset;
		u32 buf_size;
		enum xdma_direction dir;
		void *sg_transfer;	//internal type
	};

	struct xdma_transfer {
		struct dma_chan *chan;	/* (struct dma_chan *) */
		struct completion *completion;	/* (struct completion *) callback_param */

		dma_cookie_t cookie;
		u32 wait;	/* true/false */
		void *sg_transfer; /* pointer to internal SG structure */
	};


#ifdef __cplusplus
}
#endif
#endif				/* XDMA_H */
