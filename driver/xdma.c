/*
 * Wrapper Driver used to control a two-channel Xilinx DMA Engine
 */
#include <linux/dmaengine.h>
#include "xdma.h"

#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>

#include <asm/uaccess.h>
#include <linux/dma-mapping.h>

#include <linux/slab.h>
#include <linux/dma/xilinx_dma.h>
#include <linux/platform_device.h>

//#define DEBUG_PRINT 1

#ifdef DEBUG_PRINT
#define PRINT_DBG(...) printk( __VA_ARGS__)
#else
#define PRINT_DBG(...)
#endif

static dev_t dev_num;		// Global variable for the device number
static struct cdev c_dev;	// Global variable for the character device structure
static struct class *cl;	// Global variable for the device class
static struct device *dma_dev;

struct xdma_sg_mem {
	struct sg_table sg_tbl;
	unsigned long npages;
	enum dma_transfer_direction dir;
};

struct xdma_kern_buf {
	void * addr;
	unsigned long dma_addr;
	size_t size;
	struct list_head desc_list;
};

static struct xdma_kern_buf *last_dma_handle;
struct kmem_cache *buf_handle_cache;

struct xdma_dev *xdma_dev_info[MAX_DEVICES + 1];
u32 num_devices;
static int opened;
static void xdma_init(void);
static void xdma_cleanup(void);

/* save a list of dma buffers so they an be deleted in case the application
 * does not free them (in case of an abnormal abort)
 */
static struct list_head desc_list;

static int xdma_open(struct inode *i, struct file *f)
{
	PRINT_DBG(KERN_DEBUG "<%s> file: open(): Running HW initialization\n",
	       MODULE_NAME);
	//Deal with multiple opens
	if (opened == 0) {
		xdma_init();
		opened = 1;
		return 0;
	} else {
		return -EBUSY;
	}
}

static int xdma_close(struct inode *i, struct file *f)
{
	PRINT_DBG(KERN_DEBUG "<%s> file: close(): Running cleanup\n", MODULE_NAME);
	if (opened == 1) {
		xdma_cleanup();
		opened = 0;
		return 0;
	} else {
		return -EBUSY;
	}
}

static ssize_t xdma_read(struct file *f, char __user * buf, size_t
			 len, loff_t * off)
{
	PRINT_DBG(KERN_DEBUG "<%s> file: read()\n", MODULE_NAME);

	return -ENOSYS;
}

static ssize_t xdma_write(struct file *f, const char __user * buf,
			  size_t len, loff_t * off)
{
	PRINT_DBG(KERN_DEBUG "<%s> file: write()\n", MODULE_NAME);
	return -ENOSYS;
}

static int xdma_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int result;
	unsigned long requested_size;
	dma_addr_t dma_handle;
	void *buffer_addr;

	requested_size = vma->vm_end - vma->vm_start;

	PRINT_DBG(MODULE_NAME "Request %lu bytes to kernel\n", requested_size);

	buffer_addr = dma_zalloc_coherent(NULL, requested_size, &dma_handle,
			GFP_KERNEL);
	if (!buffer_addr) {
		printk(KERN_ERR "<%s> Error: allocating dma memory failed\n",
				MODULE_NAME);
		return -ENOMEM;
	}

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	result = remap_pfn_range(vma, vma->vm_start,
				 virt_to_pfn(buffer_addr),
				 requested_size, vma->vm_page_prot);

	if (result) {
		printk(KERN_ERR
		       "<%s> Error: in calling remap_pfn_range: returned %d\n",
		       MODULE_NAME, result);

		return -EAGAIN;
	}

	//last_dma_handle = kmalloc(sizeof(struct xdma_kern_buf));
	last_dma_handle = kmem_cache_alloc(buf_handle_cache, GFP_KERNEL);
	last_dma_handle->addr = buffer_addr;
	last_dma_handle->dma_addr = dma_handle;
	last_dma_handle->size = requested_size;

    list_add(&last_dma_handle->desc_list, &desc_list);

	return 0;
}

struct xdma_kern_buf* xdma_get_last_kern_buff(void)
{
	return last_dma_handle;
}

//Return the size of the buffer to be reed in order to return to the user for
//unmapping the buffer from user space
static size_t xdma_release_kernel_buffer(struct xdma_kern_buf *buff_desc)
{
	size_t size = buff_desc->size;
    list_del(&buff_desc->desc_list);
    dma_free_coherent(NULL, size, buff_desc->addr, buff_desc->dma_addr);
	kmem_cache_free(buf_handle_cache, buff_desc);
	return size;
}

//TODO implement unmap in order to deallocate memory

static void xdma_get_dev_info(u32 device_id, struct xdma_dev *dev)
{
	int i;

	for (i = 0; i < MAX_DEVICES; i++) {
		if (xdma_dev_info[i]->device_id == device_id)
			break;
	}
	memcpy(dev, xdma_dev_info[i], sizeof(struct xdma_dev));
}

static enum dma_transfer_direction xdma_to_dma_direction(enum xdma_direction
							 xdma_dir)
{
	enum dma_transfer_direction dma_dir;

	switch (xdma_dir) {
	case XDMA_MEM_TO_DEV:
		dma_dir = DMA_MEM_TO_DEV;
		break;
	case XDMA_DEV_TO_MEM:
		dma_dir = DMA_DEV_TO_MEM;
		break;
	default:
		dma_dir = DMA_TRANS_NONE;
		break;
	}

	return dma_dir;
}

static void xdma_sync_callback(void *completion)
{
    PRINT_DBG("Completion callback for %p\n", completion);
	complete(completion);
}

static int xdma_prep_user_buffer(struct xdma_buf_info * buf_info)
{
	int ret, i;
	unsigned int nr_pages, start, len, n_pg;
	struct page **page_list;
	struct scatterlist *sg, *sg_start;
	struct dma_chan *chan;
	struct dma_async_tx_descriptor *tx_desc;
	struct completion *cmp;
	enum dma_transfer_direction dir;
	enum dma_ctrl_flags flags;
	dma_cookie_t cookie;
	struct xdma_sg_mem *mem;
	unsigned long cur_base;
	unsigned long offset;
	unsigned long pg_left;
	int fp_offset, pg_len;
	int first_page;

	//TODO: Free resources in case of error in order to prevent memory leaks

	mem = kzalloc(sizeof(struct xdma_sg_mem), GFP_KERNEL);
	cmp = kmalloc(sizeof(dma_cookie_t), GFP_KERNEL);
	//reuse buffer info offset as address
	start = buf_info->address;
	len = buf_info->buf_size;
	chan = (struct dma_chan *)buf_info->chan;
	dir = xdma_to_dma_direction(buf_info->dir);
	flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;

	PRINT_DBG(KERN_DEBUG "Pinning buffer @%x;%u\n", start, len);

	//Check that the address is valid
	if (!access_ok(void, start, len)) {
		printk(KERN_DEBUG "<%s> Cannot access buffer @%x:%u\n",
				MODULE_NAME, start, len);
		return -EFAULT;
	}
	if (len == 0) {
		printk(KERN_DEBUG "<%s> Trying to transfer buffer with length 0 @%x\n",
				MODULE_NAME, start);
		return -EINVAL;
	}

	page_list = (struct page **) __get_free_page(GFP_KERNEL);
	if (!page_list) {
		kfree(mem);
		kfree(cmp);
		printk(KERN_WARNING "<%s> Unable to allocate page list for buffer %x\n",
				MODULE_NAME, start);
		return -ENOMEM;
	}
	offset = start & ~PAGE_MASK;
	nr_pages = ((((start + len -1) & PAGE_MASK) - (start & PAGE_MASK)) >> PAGE_SHIFT) + 1;
	PRINT_DBG("Pinning %u pages @%x+%lu\n", nr_pages, start, offset);

	ret = sg_alloc_table(&mem->sg_tbl, nr_pages, GFP_KERNEL);
	if (ret) {
		printk("<%s> Coud not allocate SG table for buffer %x\n",
				MODULE_NAME, start);
		return -ENOMEM;
	}

	sg_start = mem->sg_tbl.sgl;
	cur_base = start;
	pg_left = nr_pages;
	first_page = 1;
	while (pg_left) {
		n_pg = min_t(unsigned long, pg_left,
				PAGE_SIZE / sizeof(struct page *));
		ret = get_user_pages_fast(cur_base, n_pg, 1, page_list);
		PRINT_DBG("%d\n", ret);
		if (ret < 0) {
			//FIXME: free resources in case of error
			printk(KERN_ERR "Error getting user pages from %lu\n", cur_base);
			return ret;
		}

		cur_base += ret*PAGE_SIZE;
		pg_left -= ret;

		for_each_sg(sg_start, sg, ret, i) {
			fp_offset = 0;
			pg_len = PAGE_SIZE;
			//Set offset for first page
			if (first_page) {
				fp_offset = offset;
				pg_len -= offset;
				first_page = 0;
			}
			//Set size for last page
			if (pg_left == 0 && i == ret-1) {
				pg_len -= PAGE_SIZE - ((len + offset) % PAGE_SIZE);
			}
			sg_set_page(sg, page_list[i], pg_len, fp_offset);
		}
		sg_start = sg;
	}
	PRINT_DBG("Mapping %u pages and preparing transfer\n", nr_pages);
	dma_map_sg(dma_dev, mem->sg_tbl.sgl, nr_pages, dir);
	tx_desc = dmaengine_prep_slave_sg(chan, mem->sg_tbl.sgl, nr_pages, dir, flags);

	free_page((unsigned long)page_list);

	//submit transfer
	init_completion(cmp);
	tx_desc->callback = xdma_sync_callback;
	tx_desc->callback_param = cmp;
	cookie = dmaengine_submit(tx_desc);
	if (dma_submit_error(cookie)) {
		printk(KERN_ERR "<%s> Error: tx_submit error\n",
				MODULE_NAME);
		ret = -1;
	}
	buf_info->cookie = cookie;
	buf_info->completion = (u32)cmp;
	buf_info->sg_transfer = (u32)mem;

	mem->npages = nr_pages;
	mem->dir = dir;

	PRINT_DBG("Buffer prepared cmp=%p ck=%d\n", cmp, cookie);
	PRINT_DBG("buffer: %p:%d submitted\n", (void*)start, len);

	return 0;
}

static int xdma_user_buffer_release(struct xdma_sg_mem *mem)
{
	struct scatterlist *sg;
	struct page *page;
	int i;

	//TODO: Error checking
	dma_unmap_sg(dma_dev, mem->sg_tbl.sgl, mem->npages, mem->dir);

	for_each_sg(mem->sg_tbl.sgl, sg, mem->npages, i) {
		page = sg_page(sg);
		put_page(page);
	}
	sg_free_table(&mem->sg_tbl);
	kfree(mem);
	return 0;

}

static int xdma_prep_buffer(struct xdma_buf_info *buf_info)
{
	int ret = 0;
	struct dma_chan *chan;
	dma_addr_t buf;
	size_t len;
	enum dma_transfer_direction dir;
	enum dma_ctrl_flags flags;
	struct dma_async_tx_descriptor *chan_desc;
	struct completion *cmp;
	dma_cookie_t cookie;
	struct xdma_kern_buf *buf_desc;

	buf_desc = (struct xdma_kern_buf *)buf_info->address;
	chan = (struct dma_chan *)buf_info->chan;
	//cmp = (struct completion *)buf_info->completion;
    //Create a new completion for every operation
    //TODO reuse completions when possible
    //  Use a slab cache
    //Completion must be created here
    //XXX: Check if also has to be initialized here
    cmp = kmalloc(sizeof(struct completion), GFP_KERNEL);

    if (!cmp) {
        printk(KERN_ERR "Unable to allocate XDMA completion\n");
    }
    init_completion(cmp);
    buf_info->completion = (u32)cmp;
	buf_info->sg_transfer = (u32)NULL;

    //init_completion(cmp);
    //Init completion when submitting the transfer

	//TODO: Check that the buffer (or sub-buffer) does not overrun
	//the original buffer
	buf = buf_desc->dma_addr + buf_info->buf_offset;
	len = buf_info->buf_size;
	dir = xdma_to_dma_direction(buf_info->dir);

	flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;

	chan_desc = dmaengine_prep_slave_single(chan, buf, len, dir, flags);

	if (!chan_desc) {
		printk(KERN_ERR
		       "<%s> Error: dmaengine_prep_slave_single error\n",
		       MODULE_NAME);
		ret = -1;
		buf_info->cookie = -EBUSY;
	} else {
		chan_desc->callback = xdma_sync_callback;
		chan_desc->callback_param = cmp;

		// set the prepared descriptor to be executed by the engine
		//cookie = chan_desc->tx_submit(chan_desc);
        cookie = dmaengine_submit(chan_desc);
		if (dma_submit_error(cookie)) {
			printk(KERN_ERR "<%s> Error: tx_submit error\n",
			       MODULE_NAME);
			ret = -1;
		}

		buf_info->cookie = cookie;
	}
    PRINT_DBG("Buffer prepared cmp=%p\n", cmp);
    PRINT_DBG("buffer: %p:%d, %x\n", (void*)buf, len, (int)buf_desc->dma_addr);

	return ret;
}

static int xdma_start_transfer(struct xdma_transfer *trans)
{
	int ret = 0;
	unsigned long tmo = msecs_to_jiffies(3000);
	enum dma_status status;
	struct dma_chan *chan;
	struct completion *cmp;
	dma_cookie_t cookie;

	chan = (struct dma_chan *)trans->chan;
	cmp = (struct completion *)trans->completion;
	cookie = trans->cookie;

	//init_completion(cmp);
	dma_async_issue_pending(chan);
    PRINT_DBG("Submit transfer %p-%p-%d (ch-cmp-ck)", (void*)trans->chan, (void*)trans->completion, trans->cookie);

	if (trans->wait) {
        PRINT_DBG(" Sync transfer, waiting\n");
		tmo = wait_for_completion_timeout(cmp, tmo);
		status = dma_async_is_tx_complete(chan, cookie, NULL, NULL);
		if (0 == tmo) {
			printk(KERN_ERR "<%s> Error: transfer timed out\n",
			       MODULE_NAME);
			ret = -1;
		} else if (status != DMA_COMPLETE) {
			printk(KERN_DEBUG
			       "<%s> transfer: returned completion callback status of: \'%s\'\n",
			       MODULE_NAME,
			       status == DMA_ERROR ? "error" : "in progress");
			ret = -1;
		}
	}
	return ret;
}

static int xdma_finish_transfer(struct xdma_transfer *trans) {

	int ret = 0;
	unsigned long tmo = msecs_to_jiffies(3000);
	enum dma_status status;
	struct dma_chan *chan;
	struct completion *cmp;
	dma_cookie_t cookie;

	chan = (struct dma_chan *)trans->chan;
    //get the completion initialized while preparing the buffer
	cmp = (struct completion *)trans->completion;

	cookie = trans->cookie;
    PRINT_DBG("Finish transfer: Cmp/cookie: %p/%d -> done: %d\n", cmp, cookie, cmp->done);

    status = dma_async_is_tx_complete(chan, cookie, NULL, NULL);
    if (status == DMA_COMPLETE) {
        ret = XDMA_DMA_TRANSFER_FINISHED;
        //delete completion if transfer has been completed
        PRINT_DBG(" Transfer finished, deleting completion\n");
        kfree(cmp);
    } else {
        ret = XDMA_DMA_TRANSFER_PENDING;
    }

    if (trans->wait && status != DMA_COMPLETE) {
        PRINT_DBG(" Waiting for completion... %p(%d)\n", cmp, cmp->done);
        tmo = wait_for_completion_timeout(cmp, tmo);
        status = dma_async_is_tx_complete(chan, cookie, NULL, NULL);
        PRINT_DBG("  Finished t left: %lu completed: %d\n", tmo, status == DMA_COMPLETE);
        if (0 == tmo ) {
            printk(KERN_ERR "<%s> Error: transfer timed out\n",
                    MODULE_NAME);
            ret = -1;
        } else if (status != DMA_COMPLETE) {
            printk(KERN_DEBUG
                    "<%s> transfer: returned completion callback status of: \'%s\'\n",
                    MODULE_NAME,
                    status == DMA_ERROR ? "error" : "in progress");
            ret = -1;
            //We may distinguish between error or in progress
        } else {
            //may need to check if something went wrong before timeout
            ret = XDMA_DMA_TRANSFER_FINISHED;
        }
        //if wait is blocking, delete the completion
        kfree(cmp);
    }
	return ret;
}

static void xdma_stop_transfer(struct dma_chan *chan)
{
	if (chan) {
		dmaengine_terminate_all(chan);
	}
}

static long xdma_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret = 0;
	struct xdma_dev xdma_dev;
	struct xdma_buf_info buf_info;
	struct xdma_transfer trans;
	u32 devices;
	u32 chan;
	struct xdma_kern_buf *kbuff_ptr;

	switch (cmd) {
	case XDMA_GET_NUM_DEVICES:
		PRINT_DBG(KERN_DEBUG "<%s> ioctl: XDMA_GET_NUM_DEVICES\n",
		       MODULE_NAME);

		devices = num_devices;
		if (copy_to_user((u32 *) arg, &devices, sizeof(u32)))
			return -EFAULT;

		break;
	case XDMA_GET_DEV_INFO:
		PRINT_DBG(KERN_DEBUG "<%s> ioctl: XDMA_GET_DEV_INFO\n",
		       MODULE_NAME);

		if (copy_from_user((void *)&xdma_dev,
				   (const void __user *)arg,
				   sizeof(struct xdma_dev)))
			return -EFAULT;

		xdma_get_dev_info(xdma_dev.device_id, &xdma_dev);

		if (copy_to_user((struct xdma_dev *)arg,
				 &xdma_dev, sizeof(struct xdma_dev)))
			return -EFAULT;

		break;
	case XDMA_PREP_BUF:
		PRINT_DBG(KERN_DEBUG "<%s> ioctl: XDMA_PREP_BUF\n", MODULE_NAME);

		if (copy_from_user((void *)&buf_info,
				   (const void __user *)arg,
				   sizeof(struct xdma_buf_info)))
			return -EFAULT;

		ret = (long)xdma_prep_buffer(&buf_info);

		if (copy_to_user((struct xdma_buf_info *)arg,
				 &buf_info, sizeof(struct xdma_buf_info)))
			return -EFAULT;

		break;
	case XDMA_START_TRANSFER:
		PRINT_DBG(KERN_DEBUG "<%s> ioctl: XDMA_START_TRANSFER\n",
		       MODULE_NAME);

		if (copy_from_user((void *)&trans,
				   (const void __user *)arg,
				   sizeof(struct xdma_transfer)))
			return -EFAULT;

		ret = (long)xdma_start_transfer(&trans);
		break;
	case XDMA_STOP_TRANSFER:
		PRINT_DBG(KERN_DEBUG "<%s> ioctl: XDMA_STOP_TRANSFER\n",
		       MODULE_NAME);

		if (copy_from_user((void *)&chan,
				   (const void __user *)arg, sizeof(u32)))
			return -EFAULT;

		xdma_stop_transfer((struct dma_chan *)chan);
		break;
    case XDMA_FINISH_TRANSFER:
        PRINT_DBG(KERN_DEBUG "<%s> ioctl: XDMA_FINISHED_TRANSFER\n",
                MODULE_NAME);
		if (copy_from_user((void *)&trans,
				   (const void __user *)arg,
				   sizeof(struct xdma_transfer)))
			return -EFAULT;
        ret = xdma_finish_transfer(&trans);
        break;
	case XDMA_PREP_USR_BUF:
		PRINT_DBG(KERN_DEBUG "<%s> ioctl; XDMA_PREP_USR_BUFFER\n", MODULE_NAME);
		if (copy_from_user((void *)&buf_info,
					(const void __user *)arg,
					sizeof(struct xdma_buf_info)))
			return -EFAULT;

		ret = xdma_prep_user_buffer(&buf_info);

		if (copy_to_user((struct xdma_buf_info *)arg,
					&buf_info, sizeof(struct xdma_buf_info)))
			return -EFAULT;
		break;
	case XDMA_RELEASE_USR_BUF:
		PRINT_DBG(KERN_DEBUG "<%s> ioctl: XDMA_RELEASE_USR_BUFFER\n", MODULE_NAME);
		// The user parameter is already a pointer to the xdma_sg_mem structure
		ret = xdma_user_buffer_release((struct xdma_sg_mem *)arg);

		break;
	case XDMA_GET_LAST_KBUF:
		if (!access_ok(void*, arg, sizeof(void*))) {
			printk(KERN_DEBUG "<%s> Cannot access user variable @0x%lx",
					MODULE_NAME, arg);
			return -EFAULT;
		}
		kbuff_ptr = xdma_get_last_kern_buff();
		if (!kbuff_ptr)
			ret = -EFAULT;
		put_user((unsigned long)kbuff_ptr, (unsigned long __user *)arg);
		break;
	case XDMA_RELEASE_KBUF:
		PRINT_DBG(KERN_DEBUG "<%s> ioctl: XDMA_RELEASE_KBUFF\n", MODULE_NAME);
		if (!access_ok(void*, arg, sizeof(void*))) {
			printk(KERN_DEBUG "<%s> Cannot access user variable @0x%lx",
					MODULE_NAME, arg);
			return -EFAULT;
		}
		get_user(kbuff_ptr, (struct xdma_kern_buf **)arg);
		ret = xdma_release_kernel_buffer(kbuff_ptr);
		break;

	default:
        printk(KERN_DEBUG "<%s> ioctl: WARNING unknown ioctl command %d\n", MODULE_NAME, cmd);
		break;
	}

	return ret;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = xdma_open,
	.release = xdma_close,
	.read = xdma_read,
	.write = xdma_write,
	.mmap = xdma_mmap,
	.unlocked_ioctl = xdma_ioctl,
};

static bool xdma_filter(struct dma_chan *chan, void *param)
{
	printk(KERN_INFO "Filtering channel...\n");
	if (chan == NULL) {
		printk(KERN_INFO "Filtering a null channel\n");
		return false;
	}
	if (chan->private == NULL) {
		printk(KERN_INFO "Getting null channel private\n");
		return false;
	}
	if (param == NULL) {
		printk(KERN_INFO "NULL param in filter function\n");
		return false;
	}
	if (*((int *)chan->private) == *(int *)param)
		return true;

	return false;
}

static void xdma_add_dev_info(struct dma_chan *tx_chan,
			      struct dma_chan *rx_chan)
{
	struct completion *tx_cmp, *rx_cmp;

	tx_cmp = (struct completion *)
	    kzalloc(sizeof(struct completion), GFP_KERNEL);

	rx_cmp = (struct completion *)
	    kzalloc(sizeof(struct completion), GFP_KERNEL);

	xdma_dev_info[num_devices] = (struct xdma_dev *)
	    kzalloc(sizeof(struct xdma_dev), GFP_KERNEL);

	xdma_dev_info[num_devices]->tx_chan = (u32) tx_chan;
	xdma_dev_info[num_devices]->tx_cmp = (u32) tx_cmp;

	xdma_dev_info[num_devices]->rx_chan = (u32) rx_chan;
	xdma_dev_info[num_devices]->rx_cmp = (u32) rx_cmp;

	xdma_dev_info[num_devices]->device_id = num_devices;
	num_devices++;
}

static void xdma_init(void)
{
	dma_cap_mask_t mask;
	u32 match_tx, match_rx;
	struct dma_chan *tx_chan, *rx_chan;

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE | DMA_PRIVATE, mask);

	for (;;) {
		match_tx = (DMA_MEM_TO_DEV & 0xFF) | XILINX_DMA_IP_DMA |
		    (num_devices << XILINX_DMA_DEVICE_ID_SHIFT);

		tx_chan = dma_request_channel(mask, xdma_filter,
					      (void *)&match_tx);

		match_rx = (DMA_DEV_TO_MEM & 0xFF) | XILINX_DMA_IP_DMA |
		    (num_devices << XILINX_DMA_DEVICE_ID_SHIFT);

		rx_chan = dma_request_channel(mask, xdma_filter,
					      (void *)&match_rx);

		if (!tx_chan && !rx_chan) {
			printk(KERN_DEBUG
			       "<%s> probe: number of devices found: %d\n",
			       MODULE_NAME, num_devices);
			break;
		} else {
			xdma_add_dev_info(tx_chan, rx_chan);
		}
	}
	//slab cache for the buffer descriptors
	buf_handle_cache = kmem_cache_create("DMA buffer descriptor cache",
			sizeof(struct xdma_kern_buf),
			0, 0, NULL);
}

static void xdma_cleanup(void)
{
	int i;
    struct xdma_kern_buf *bdesc;
	num_devices = 0;

	for (i = 0; i < MAX_DEVICES; i++) {
		if (xdma_dev_info[i]) {
			if (xdma_dev_info[i]->tx_chan)
				dma_release_channel((struct dma_chan *)
						    xdma_dev_info[i]->tx_chan);

			if (xdma_dev_info[i]->tx_cmp)
				kfree((struct completion *)
				      xdma_dev_info[i]->tx_cmp);

			if (xdma_dev_info[i]->rx_chan)
				dma_release_channel((struct dma_chan *)
						    xdma_dev_info[i]->rx_chan);

			if (xdma_dev_info[i]->rx_cmp)
				kfree((struct completion *)
				      xdma_dev_info[i]->rx_cmp);
		}
	}

    //free all allocated dma buffers
    while (!list_empty(&desc_list)) {
        bdesc = list_first_entry(&desc_list, struct xdma_kern_buf, desc_list);
        //this frees the buffer and deletes its descriptor from the list
        xdma_release_kernel_buffer(bdesc);
    }
	kmem_cache_destroy(buf_handle_cache);
}

static int __init xdma_probe(void)
{
	num_devices = 0;

	/* device constructor */
	printk(KERN_DEBUG "<%s> init: registered\n", MODULE_NAME);
	if (alloc_chrdev_region(&dev_num, 0, 1, MODULE_NAME) < 0) {
		return -1;
	}
	if ((cl = class_create(THIS_MODULE, MODULE_NAME)) == NULL) {
		unregister_chrdev_region(dev_num, 1);
		return -1;
	}

	dma_dev = device_create(cl, NULL, dev_num, NULL, MODULE_NAME);
	if (dma_dev == NULL) {
		class_destroy(cl);
		unregister_chrdev_region(dev_num, 1);
		return -1;
	}
	cdev_init(&c_dev, &fops);
	if (cdev_add(&c_dev, dev_num, 1) == -1) {
		device_destroy(cl, dev_num);
		class_destroy(cl);
		unregister_chrdev_region(dev_num, 1);
		return -1;
	}

    INIT_LIST_HEAD(&desc_list);

	return 0;
}

static void __exit xdma_exit(void)
{
	/* device destructor */
	cdev_del(&c_dev);
	device_destroy(cl, dev_num);
	class_destroy(cl);
	unregister_chrdev_region(dev_num, 1);
	printk(KERN_DEBUG "<%s> exit: unregistered\n", MODULE_NAME);
}

module_init(xdma_probe);
module_exit(xdma_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Wrapper Driver For A Xilinx DMA Engine");
