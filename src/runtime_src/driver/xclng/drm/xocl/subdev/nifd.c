#define pr_fmt(fmt)     KBUILD_MODNAME ":%s: " fmt, __func__

#include "../xocl_drv.h"

static int nifd_open(struct inode *inode, struct file *file);

static int nifd_close(struct inode *inode, struct file *file);

static long nifd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

static int nifd_probe(struct platform_device *pdev);

static int nifd_remove(struct platform_device *pdev);

void xocl_fini_nifd(void);

int __init xocl_init_nifd(void);

struct xocl_nifd
{
    void *__iomem base_nifd;
    void *__iomem base_icap;
    unsigned int instance;
    struct cdev sys_cdev;
    struct device *sys_device;
};

static const struct file_operations nifd_fops = {
    .owner = THIS_MODULE,
    .open = nifd_open,
    .release = nifd_close,
    .unlocked_ioctl = nifd_ioctl};

struct platform_device_id nifd_id_table[] = {
    {XOCL_NIFD, 0},
    {},
};

static struct platform_driver nifd_driver = {
    .probe = nifd_probe,
    .remove = nifd_remove,
    .driver = {.name = XOCL_NIFD},
    .id_table = nifd_id_table,
};

static int nifd_open(struct inode *inode, struct file *file) {
    return 0;
}

static int nifd_close(struct inode *inode, struct file *file) {
    return 0;
}

static int nifd_probe(struct platform_device *pdev) {
    struct xocl_nifd *nifd;
    struct resource *res;
    struct xocl_dev_core *core;
    int err;
    nifd = devm_kzalloc(&pdev->dev, sizeof(*nifd), GFP_KERNEL);
    if (!nifd) {
        return -ENOMEM;
    }
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    nifd->base_nifd = ioremap_nocache(res->start, res->end - res->start + 1);
    if (!nifd->base_nifd) {
        xocl_err(&pdev->dev, "Map iomem failed");
        return -EIO;
    }
    // TODO: add icap address
    core = xocl_get_xdev(pdev);
    if (!core) {
        return -1;
    }
    return 0;
}

static int nifd_remove(struct platform_device *pdev) {
    struct xocl_dev_core *core;
    core = xocl_get_xdev(pdev);
    printk("NIFD: checking core in remove");
    if (!core) {
        printk("NIFD: core is null in remove");
        return -1;
    }
    device_destroy(xrt_class, nifd->sys_cdev.dev);
    cdev_del(&nifd->sys_cdev);
    iounmap(nifd->base_nifd);
    devm_kfree(&pdev->dev, nifd);
    return 0;
}

int __init xocl_init_nifd(void) {
    int err = 0;
    printk("NIFD: alloc_chrdev_region start");
    err = alloc_chrdev_region(&nifd_dev, 0, 1, XOCL_NIFD);
    printk("NIFD: alloc_chrdev_region return");
    if (err < 0) {
        printk("NIFD: alloc_chrdev_region err");
        return err;
    }
    printk("NIFD: platform_driver_register start");
    err = platform_driver_register(&nifd_driver);
    printk("NIFD: platform_driver_register return");
    if (err) {
        printk("NIFD: platform_driver_register err");
        unregister_chrdev_region(nifd_dev, 1);
        return err;
    }
    return 0;
}

void xocl_fini_nifd(void)
{
    unregister_chrdev_region(nifd_dev, 1);
    platform_driver_unregister(&nifd_driver);
}