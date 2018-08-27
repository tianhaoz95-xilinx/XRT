/*
 * Copyright (C) 2018 Xilinx, Inc. All rights reserved.
 *
 * Authors: Lizhi.Hou@Xilinx.com
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* QDMA stream */
#include <linux/version.h>
#include <linux/anon_inodes.h>
#include <linux/dma-buf.h>
#include <linux/aio.h>
#include <linux/uio.h>
#include "../xocl_drv.h"
#include "../userpf/common.h"
#include "../userpf/xocl_bo.h"
#include "../lib/libqdma/libqdma_export.h"
#include "../lib/libqdma/qdma_wq.h"
#include "qdma_ioctl.h"

#define	PROC_TABLE_HASH_SZ	512
#define	EBUF_LEN		256
#define	MINOR_NAME_MASK		0xffff

#define	QUEUE_POST_TIMEOUT	10000

static dev_t	str_dev;

struct stream_async_arg {
	struct stream_queue	*queue;
	struct drm_xocl_unmgd	unmgd;
	u32			nsg;
	struct drm_xocl_bo	*xobj;
	bool			is_unmgd;
	struct kiocb		*kiocb;
};

struct stream_queue {
	struct qdma_wq		queue;
	u32			state;
	struct file		*file;
	int			qfd;
	int			refcnt;
	struct str_device	*sdev;

	u64			trans_bytes;
};

struct str_device {
	struct platform_device  *pdev;
	struct cdev		cdev;
	struct device		*sys_device;

	struct mutex		str_dev_lock;

	u16			instance;

	struct qdma_dev_conf	*dev_info;
};

static u64 get_str_stat(struct platform_device *pdev, u32 q_idx)
{
	struct str_device *sdev;

	sdev = platform_get_drvdata(pdev);
	BUG_ON(!sdev);

	return 0;
}

static struct xocl_str_dma_funcs str_ops = {
	.get_str_stat = get_str_stat,
};

static const struct vm_operations_struct stream_vm_ops = {
	.fault = xocl_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static int queue_wqe_complete(struct qdma_complete_event *compl_event)
{
	struct stream_async_arg *cb_arg;
	enum dma_data_direction dir;
	struct kiocb		*kiocb;
	struct xocl_dev		*xdev;

	cb_arg = (struct stream_async_arg *)compl_event->req_priv;
	kiocb = cb_arg->kiocb;

	if (cb_arg->is_unmgd) {
		xdev = xocl_get_xdev(cb_arg->queue->sdev->pdev);
		dir = cb_arg->queue->queue.qconf->c2h ? DMA_FROM_DEVICE :
			DMA_TO_DEVICE;
		pci_unmap_sg(xdev->core.pdev, cb_arg->unmgd.sgt->sgl,
			cb_arg->nsg, dir);
		xocl_finish_unmgd(&cb_arg->unmgd);
	} else {
		drm_gem_object_unreference_unlocked(&cb_arg->xobj->base);
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,16,0)
        kiocb->ki_complete(kiocb, compl_event->done_bytes,
		compl_event->error);
#else
        aio_complete(kiocb, compl_event->done_bytes, compl_event->error);
#endif

	return 0;
}

static ssize_t stream_post_bo(struct str_device *sdev,
	struct stream_queue *queue, struct drm_gem_object *gem_obj,
	loff_t offset, size_t len, bool write, struct kiocb *kiocb)
{
	struct drm_xocl_bo *xobj;
	struct xocl_dev *xdev;
	struct qdma_wr wr;
	struct stream_async_arg cb_arg;
	ssize_t ret;

	xdev = xocl_get_xdev(sdev->pdev);
	if (gem_obj->size < offset + len) {
		xocl_err(&sdev->pdev->dev, "Invalid request, buf size: %ld, "
			"request size %ld, offset %lld",
			gem_obj->size, len, offset);
		ret = -EINVAL;
		goto failed;
	}

	drm_gem_object_reference(gem_obj);
	xobj = to_xocl_bo(gem_obj);

	memset(&wr, 0, sizeof (wr));
	wr.write = write;
	wr.len = len;
	wr.sgt = xobj->sgt;
	if (kiocb) {
		cb_arg.is_unmgd = false;
		cb_arg.kiocb = kiocb;
		cb_arg.xobj = xobj;
		cb_arg.queue = queue;
		wr.priv_data = &cb_arg;
		wr.complete = queue_wqe_complete;
	} else {
		wr.block = true;
	}


	ret = qdma_wq_post(&queue->queue, &wr);
	if (ret < 0) {
		xocl_err(&sdev->pdev->dev, "post wr failed ret=%ld", ret);
	}

failed:
	if (wr.block) {
		drm_gem_object_unreference_unlocked(gem_obj);
	}

	return ret;
}

static ssize_t queue_rw(struct str_device *sdev, struct stream_queue *queue,
	char __user *buf, size_t sz, bool write, struct kiocb *kiocb)
{
	struct vm_area_struct	*vma;
	struct xocl_dev *xdev;
	struct drm_xocl_unmgd unmgd;
	unsigned long buf_addr = (unsigned long)buf;
	struct stream_async_arg cb_arg;
	enum dma_data_direction dir;
	u32 nents;
	struct qdma_wr wr;
	long	ret = 0;

	xocl_info(&sdev->pdev->dev, "Read / Write Queue %ld",
		queue->queue.qhdl);

	if (sz == 0)
		return 0;

	if (((uint64_t)(buf) & ~PAGE_MASK) && queue->queue.qconf->c2h) {
		xocl_err(&sdev->pdev->dev,
			"C2H buffer has to be page aligned, buf %p", buf);
		ret = -EINVAL;
		goto failed;
	}

	xdev = xocl_get_xdev(sdev->pdev);

	vma = find_vma(current->mm, buf_addr);
	if (vma && (vma->vm_ops == &stream_vm_ops)) {
		if (vma->vm_start > buf_addr || vma->vm_end <= buf_addr + sz) {
			return -EINVAL;
		}
		ret = stream_post_bo(sdev, queue, vma->vm_private_data,
			(buf_addr - vma->vm_start), sz, write, kiocb);
		return ret;
	}

	ret = xocl_init_unmgd(&unmgd, (uint64_t)buf, sz, write);
	if (ret) {
		xocl_err(&sdev->pdev->dev, "Init unmgd buf failed, "
			"ret=%ld", ret);
		goto failed;
	}

	dir = write ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	nents = pci_map_sg(xdev->core.pdev, unmgd.sgt->sgl,
		unmgd.sgt->orig_nents, dir);
	if (!nents) {
		xocl_err(&sdev->pdev->dev, "map sgl failed");
		ret = -EFAULT;
		xocl_finish_unmgd(&unmgd);
		goto failed;
	}

	memset(&wr, 0, sizeof (wr));
	wr.write = write;
	wr.len = sz;
	wr.sgt = unmgd.sgt;

	if (kiocb) {
		memcpy(&cb_arg.unmgd, &unmgd, sizeof (unmgd));
		cb_arg.is_unmgd = true;
		cb_arg.queue = queue;
		cb_arg.kiocb = kiocb;
		cb_arg.nsg = nents;
		wr.priv_data = &cb_arg;
		wr.complete = queue_wqe_complete;
	} else {
		wr.block = true;
	}

	ret = qdma_wq_post(&queue->queue, &wr);

	if (wr.block) {
		pci_unmap_sg(xdev->core.pdev, unmgd.sgt->sgl, nents, dir);
		xocl_finish_unmgd(&unmgd);
	}

failed:
	return ret;
}

static ssize_t queue_read(struct file *filp, char __user *buf, size_t sz,
	loff_t *off)
{
	struct stream_queue	*queue;
	struct str_device	*sdev;

	queue = (struct stream_queue *)filp->private_data;
	sdev = queue->sdev;

	return queue_rw(sdev, queue, buf, sz, false, NULL);
}

static ssize_t queue_write(struct file *filp, const char __user *buf, size_t sz,
	loff_t *off)
{
	struct stream_queue	*queue;
	struct str_device	*sdev;

	queue = (struct stream_queue *)filp->private_data;
	sdev = queue->sdev;

	return queue_rw(sdev, queue, (char *)buf, sz, true, NULL);
}

static int queue_wqe_cancel(struct kiocb *kiocb)
{
	struct stream_queue	*queue;

	queue = (struct stream_queue *)kiocb->ki_filp->private_data;

	return qdma_cancel_req(&queue->queue);
}

static ssize_t queue_aio_read(struct kiocb *kiocb, const struct iovec *iov,
	unsigned long nr, loff_t off)
{
	struct stream_queue	*queue;
	struct str_device	*sdev;
	int			i;
	ssize_t			total = 0, ret = 0;

	queue = (struct stream_queue *)kiocb->ki_filp->private_data;
	sdev = queue->sdev;

	kiocb_set_cancel_fn(kiocb, queue_wqe_cancel);

	for (i = 0; i < nr; i++) {
		ret = queue_rw(sdev, queue, iov[i].iov_base, iov[i].iov_len,
			false, kiocb);
		if (ret < 0) {
			break;
		}
		total += ret;
	}

	return total > 0 ? -EIOCBQUEUED : ret;
}

static ssize_t queue_aio_write(struct kiocb *kiocb, const struct iovec *iov,
	unsigned long nr, loff_t off)
{
	struct stream_queue	*queue;
	struct str_device	*sdev;
	int			i;
	ssize_t			total = 0, ret = 0;

	queue = (struct stream_queue *)kiocb->ki_filp->private_data;
	sdev = queue->sdev;

	kiocb_set_cancel_fn(kiocb, queue_wqe_cancel);

	for (i = 0; i < nr; i++) {
		ret = queue_rw(sdev, queue, iov[i].iov_base, iov[i].iov_len,
			true, kiocb);
		if (ret < 0) {
			break;
		}
		total += ret;
	}

	return total > 0 ? -EIOCBQUEUED : ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,16,0)
static ssize_t queue_write_iter(struct kiocb *kiocb, struct iov_iter *io)
{
	struct stream_queue	*queue;
	struct str_device	*sdev;
	unsigned long		i, nr;
	ssize_t			total = 0, ret = 0;


	queue = (struct stream_queue *)kiocb->ki_filp->private_data;
	sdev = queue->sdev;

	nr = io->nr_segs;
	if (!iter_is_iovec(io) || !nr) {
		xocl_err(&sdev->pdev->dev, "Invalid request nr = %ld", nr);
		goto end;
	}
		
	if (!is_sync_kiocb(kiocb)) {
		ret = queue_aio_write(kiocb, io->iov, nr, io->iov_offset);
		goto end;
	}

	for (i = 0; i < nr; i++) {
		ret = queue_rw(sdev, queue, io->iov[i].iov_base,
			io->iov[i].iov_len, true, NULL);
		if (ret < 0) {
			break;
		}
		total += ret;
	}

	ret = total > 0 ? total : ret;

end:
	return ret;
}

static ssize_t queue_read_iter(struct kiocb *kiocb, struct iov_iter *io)
{
	struct stream_queue	*queue;
	struct str_device	*sdev;
	unsigned long		i, nr;
	ssize_t			total = 0, ret = 0;


	queue = (struct stream_queue *)kiocb->ki_filp->private_data;
	sdev = queue->sdev;

	nr = io->nr_segs;
	if (!iter_is_iovec(io) || !nr) {
		xocl_err(&sdev->pdev->dev, "Invalid request nr = %ld", nr);
		goto end;
	}
		
	if (!is_sync_kiocb(kiocb)) {
		ret = queue_aio_read(kiocb, io->iov, nr, io->iov_offset);
		goto end;
	}

	for (i = 0; i < nr; i++) {
		ret = queue_rw(sdev, queue, io->iov[i].iov_base,
			io->iov[i].iov_len, false, NULL);
		if (ret < 0) {
			break;
		}
		total += ret;
	}

	ret = total > 0 ? total : ret;

end:
	return ret;
}
#endif

static int queue_release(struct inode *inode, struct file *file)
{
	struct str_device *sdev;
	struct stream_queue *queue;
	struct xocl_dev *xdev;
	long	ret = 0;

	queue = (struct stream_queue *)file->private_data;
	sdev = queue->sdev;

	xdev = xocl_get_xdev(sdev->pdev);

	xocl_info(&sdev->pdev->dev, "Release Queue %ld", queue->queue.qhdl);

	if (queue->refcnt > 0) {
		xocl_err(&sdev->pdev->dev, "Queue is busy");
		return -EBUSY;
	}

	ret = qdma_wq_destroy(&queue->queue);
	if (ret < 0) {
		xocl_err(&sdev->pdev->dev,
			"Destroy queue failed ret = %ld", ret);
		goto failed;
	}

	devm_kfree(&sdev->pdev->dev, queue);
		
failed:
	return ret;
}

static struct file_operations queue_fops = {
	.owner = THIS_MODULE,
	.read = queue_read,
	.write = queue_write,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,16,0)
	.write_iter = queue_write_iter,
	.read_iter = queue_read_iter,
#else
	.aio_read = queue_aio_read,
	.aio_write = queue_aio_write,
#endif
	.release = queue_release,
};

static long stream_ioctl_create_queue(struct str_device *sdev,
	void __user *arg)
{
	struct xocl_qdma_ioc_create_queue req;
	struct qdma_queue_conf qconf;
	struct xocl_dev *xdev;
	struct stream_queue *queue;
	long	ret;

	if (copy_from_user((void *)&req, arg,
		sizeof (struct xocl_qdma_ioc_create_queue))) {
		xocl_err(&sdev->pdev->dev, "copy failed.");
		return -EFAULT;
	}

	queue = devm_kzalloc(&sdev->pdev->dev, sizeof (*queue), GFP_KERNEL);
	if (!queue) {
		xocl_err(&sdev->pdev->dev, "out of memeory");
		return -ENOMEM;
	}
	queue->qfd = -1;

	xdev = xocl_get_xdev(sdev->pdev);

	memset(&qconf, 0, sizeof (qconf));
	qconf.st = 1; /* stream queue */
	qconf.qidx = QDMA_QUEUE_IDX_INVALID; /* request libqdma to alloc */
	qconf.wbk_en =1; 
        qconf.wbk_acc_en=1; 
        qconf.wbk_pend_chk=1;
        qconf.fetch_credit=1; 
        qconf.cmpl_stat_en=1;
        qconf.cmpl_trig_mode=1;

	if (!req.write)
		qconf.c2h = 1;
	ret = qdma_wq_create((unsigned long)xdev->dma_handle, &qconf,
		&queue->queue, sizeof (struct stream_async_arg));
	if (ret < 0) {
		xocl_err(&sdev->pdev->dev, "Creating Queue failed ret = %ld",
			ret);
		goto failed;
	}

	xocl_info(&sdev->pdev->dev, "Created Queue handle %ld, index %d, sz %d",
		queue->queue.qhdl, queue->queue.qconf->qidx,
		queue->queue.qconf->rngsz);

	queue->file = anon_inode_getfile("qdma_queue", &queue_fops, queue,
		O_CLOEXEC | O_RDWR);
	if (!queue->file) {
		ret = -EFAULT;
		goto failed;
	}
	queue->file->private_data = queue;
	queue->qfd = get_unused_fd_flags(0); /* e.g. O_NONBLOCK */
	if (queue->qfd < 0) {
		ret = -EFAULT;
		xocl_err(&sdev->pdev->dev, "Failed get fd");
		goto failed;
	}
	fd_install(queue->qfd, queue->file);
	req.handle = queue->qfd;

	if (copy_to_user(arg, &req, sizeof (req))) {
		xocl_err(&sdev->pdev->dev, "Copy to user failed");
		ret = -EFAULT;
		goto failed;
	}

	queue->sdev = sdev;

	return 0;

failed:
	if (queue->qfd >= 0)
		put_unused_fd(queue->qfd);

	if (queue->file) {
		fput(queue->file);
		queue->file = NULL;
	}

	if (queue) {
		devm_kfree(&sdev->pdev->dev, queue);
	}

	qdma_wq_destroy(&queue->queue);

	return ret;
}

static long stream_ioctl_alloc_buffer(struct str_device *sdev,
	void __user *arg)
{
	struct xocl_qdma_ioc_alloc_buf req;
	struct xocl_dev *xdev;
	struct drm_xocl_bo *xobj;
	struct dma_buf *dmabuf = NULL;
	int flags;
	int ret;

	if (copy_from_user((void *)&req, arg,
		sizeof (struct xocl_qdma_ioc_alloc_buf))) {
		xocl_err(&sdev->pdev->dev, "copy failed.");
		return -EFAULT;
	}

	xdev = xocl_get_xdev(sdev->pdev);

	xobj = xocl_create_bo(xdev->ddev, req.size, 0, DRM_XOCL_BO_EXECBUF);
	if (IS_ERR(xobj)) {
		ret = PTR_ERR(xobj);
		xocl_err(&sdev->pdev->dev, "create bo failed");
		return ret;
	}

	xobj->pages = drm_gem_get_pages(&xobj->base);
	if (IS_ERR(xobj->pages)) {
		ret = PTR_ERR(xobj->pages);
		xocl_err(&sdev->pdev->dev, "Get pages failed");
		goto failed;
	}

	xobj->sgt = drm_prime_pages_to_sg(xobj->pages,
		xobj->base.size >> PAGE_SHIFT);
	if (IS_ERR(xobj->sgt)) {
		ret = PTR_ERR(xobj->sgt);
		goto failed;
	}

	xobj->vmapping = vmap(xobj->pages, xobj->base.size >> PAGE_SHIFT,
		VM_MAP, PAGE_KERNEL);
	if (!xobj->vmapping) {
		ret = -ENOMEM;
		goto failed;
	}

	xobj->dma_nsg = pci_map_sg(xdev->core.pdev, xobj->sgt->sgl,
		xobj->sgt->orig_nents, PCI_DMA_BIDIRECTIONAL);
	if (!xobj->dma_nsg) {
		xocl_err(&sdev->pdev->dev, "map sgl failed, sgt");
		ret = -EIO;
		goto failed;
	}

	ret = drm_gem_create_mmap_offset(&xobj->base);
	if (ret < 0)
		goto failed;

	flags = O_CLOEXEC | O_RDWR;

	drm_gem_object_reference(&xobj->base);
	dmabuf = drm_gem_prime_export(xdev->ddev, &xobj->base, flags);
	if (IS_ERR(dmabuf)) {
		xocl_err(&sdev->pdev->dev, "failed to export dma_buf");
		ret = PTR_ERR(dmabuf);
		goto failed;
	}
	xobj->dmabuf = dmabuf;
	xobj->dmabuf_vm_ops = &stream_vm_ops;

	req.buf_fd = dma_buf_fd(dmabuf, flags);
	if (req.buf_fd < 0) {
		goto failed;
	}

	if (copy_to_user(arg, &req, sizeof (req))) {
		xocl_err(&sdev->pdev->dev, "Copy to user failed");
		ret = -EFAULT;
		goto failed;
	}

	return 0;
failed:
	if (req.buf_fd >= 0) {
		put_unused_fd(req.buf_fd);
	}

	if (!IS_ERR(dmabuf)) {
		dma_buf_put(dmabuf);
	}

	if (xobj) {
		xocl_free_bo(&xobj->base);
	}

	return ret;
}

static long stream_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct str_device	*sdev;
	long result = 0;

	sdev = (struct str_device *)filp->private_data;

	switch (cmd) {
	case XOCL_QDMA_IOC_CREATE_QUEUE:
		result = stream_ioctl_create_queue(sdev, (void __user *)arg);
		break;
	case XOCL_QDMA_IOC_ALLOC_BUFFER:
		result = stream_ioctl_alloc_buffer(sdev, (void __user *)arg);
		break;
	default:
		xocl_err(&sdev->pdev->dev, "Invalid request %u", cmd & 0xff);
		result = -EINVAL;
		break;
	}

	return result;
}

static int stream_open(struct inode *inode, struct file *file)
{
	struct str_device *sdev;

	sdev = container_of(inode->i_cdev, struct str_device, cdev);

	file->private_data = sdev;

	xocl_info(&sdev->pdev->dev, "opened file %p by pid: %d",
		file, pid_nr(task_tgid(current)));

	return 0;
}

static int stream_close(struct inode *inode, struct file *file)
{
	struct str_device *sdev;
	struct xocl_dev *xdev;

	sdev = (struct str_device *)file->private_data;

	xdev = xocl_get_xdev(sdev->pdev);


	xocl_info(&sdev->pdev->dev, "Closing file %p by pid: %d",
		file, pid_nr(task_tgid(current)));

	return 0;
}

/*
 * char device for QDMA
 */
static const struct file_operations stream_fops = {
	.owner = THIS_MODULE,
	.open = stream_open,
	.release = stream_close,
	.unlocked_ioctl = stream_ioctl,
};

static int str_dma_probe(struct platform_device *pdev)
{
	struct str_device	*sdev = NULL;
	struct xocl_dev		*xdev;
	char			ebuf[EBUF_LEN + 1];
	int			ret = 0;

	sdev = devm_kzalloc(&pdev->dev, sizeof (*sdev), GFP_KERNEL);
	if (!sdev) {
		xocl_err(&pdev->dev, "alloc stream dev failed");
		ret = -ENOMEM;
		goto failed;
	}

	sdev->pdev = pdev;
	xdev = xocl_get_xdev(pdev);

	sdev->dev_info = qdma_device_get_config((unsigned long)xdev->dma_handle,
		ebuf, EBUF_LEN);
	if (!sdev->dev_info) {
		xocl_err(&pdev->dev, "Failed to get device info");
		ret = -EINVAL;
		goto failed;
	}

	cdev_init(&sdev->cdev, &stream_fops);
	sdev->cdev.owner = THIS_MODULE;
	sdev->instance = XOCL_DEV_ID(xdev->core.pdev);
	sdev->cdev.dev = MKDEV(MAJOR(str_dev), sdev->instance);
	ret = cdev_add(&sdev->cdev, sdev->cdev.dev, 1);
	if (ret) {
		xocl_err(&pdev->dev, "failed cdev_add, ret=%d", ret);
		goto failed;
	}

	sdev->sys_device = device_create(xrt_class, &pdev->dev,
		sdev->cdev.dev, NULL, "%s%d",
		platform_get_device_id(pdev)->name,
		sdev->instance & MINOR_NAME_MASK);
	if (IS_ERR(sdev->sys_device)) {
		ret = PTR_ERR(sdev->sys_device);
		xocl_err(&pdev->dev, "failed to create cdev");
		goto failed_create_cdev;
	}

	mutex_init(&sdev->str_dev_lock);
	
	xocl_subdev_register(pdev, XOCL_SUBDEV_STR_DMA, &str_ops);
	platform_set_drvdata(pdev, sdev);

	return 0;

failed_create_cdev:
	cdev_del(&sdev->cdev);
failed:
	if (sdev) {
		devm_kfree(&pdev->dev, sdev);
	}

	platform_set_drvdata(pdev, NULL);

	return ret;
}

static int str_dma_remove(struct platform_device *pdev)
{
	struct str_device *sdev = platform_get_drvdata(pdev);

	if (!sdev) {
		xocl_err(&pdev->dev, "driver data is NULL");
		return -EINVAL;
	}

	if (sdev->sys_device)
		device_destroy(xrt_class, sdev->cdev.dev);
	devm_kfree(&pdev->dev, sdev);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_device_id str_dma_id_table[] = {
	{ XOCL_STR_QDMA, 0 },
	{ },
};

static struct platform_driver	str_dma_driver = {
	.probe		= str_dma_probe,
	.remove		= str_dma_remove,
	.driver		= {
		.name = "xocl_str_qdma",
	},
	.id_table = str_dma_id_table,
};

int __init xocl_init_str_qdma(void)
{
	int		err = 0;

	err = alloc_chrdev_region(&str_dev, 0, XOCL_CHARDEV_REG_COUNT,
		XOCL_STR_QDMA);
	if (err < 0)
		goto err_reg_chrdev;

	err = platform_driver_register(&str_dma_driver);
	if (err) {
		goto err_drv_reg;
	}

	return 0;

err_drv_reg:
	unregister_chrdev_region(str_dev, XOCL_CHARDEV_REG_COUNT);
err_reg_chrdev:
	return err;
}

void xocl_fini_str_qdma(void)
{
	unregister_chrdev_region(str_dev, XOCL_CHARDEV_REG_COUNT);
	platform_driver_unregister(&str_dma_driver);
}
