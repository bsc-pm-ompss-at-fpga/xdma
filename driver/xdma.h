#ifndef XDMA_H
#define XDMA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <linux/types.h>
#include <linux/list.h>
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
		u32 tx_chan;	/* (struct dma_chan *) */
		u32 tx_cmp;	/* (struct completion *) callback_param */
		u32 rx_chan;	/* (struct dma_chan *) */
		u32 rx_cmp;	/* (struct completion *) callback_param */
		u32 device_id;
	};

	struct xdma_chan_cfg {
		u32 chan;	/* (struct dma_chan *) */

		enum xdma_direction dir;	/* Channel direction */
		int coalesc;	/* Interrupt coalescing threshold */
		int delay;	/* Delay counter */
		int reset;	/* Reset Channel */
	};

	struct xdma_buf_info {
		u32 chan;	/* (struct dma_chan *) */
		u32 completion;	/* (struct completion *) callback_param */

		dma_cookie_t cookie;
		u32 address;
		u32 buf_offset;
		u32 buf_size;
		enum xdma_direction dir;
		u32 sg_transfer;
	};

	struct xdma_transfer {
		u32 chan;	/* (struct dma_chan *) */
		u32 completion;	/* (struct completion *) callback_param */

		dma_cookie_t cookie;
		u32 wait;	/* true/false */
        u32 sg_transfer; /* pointer to internal SG structure */
	};

	struct xdma_kern_buf {
		void * addr;
		unsigned long dma_addr;
		size_t size;
        struct list_head desc_list;
	};

#ifdef __cplusplus
}
#endif
#endif				/* XDMA_H */
