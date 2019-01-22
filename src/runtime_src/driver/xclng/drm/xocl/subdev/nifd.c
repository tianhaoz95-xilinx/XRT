#define pr_fmt(fmt)     KBUILD_MODNAME ":%s: " fmt, __func__

#include "../xocl_drv.h"

static dev_t nifd_dev;

static long nifd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

static int nifd_open(struct inode *inode, struct file *file);

static int nifd_close(struct inode *inode, struct file *file);

static long nifd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

static int nifd_probe(struct platform_device *pdev);

static int nifd_remove(struct platform_device *pdev);

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

static long nifd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    return 0;
}

static int nifd_open(struct inode *inode, struct file *file) {
    return 0;
}

static int nifd_close(struct inode *inode, struct file *file) {
    return 0;
}

static int nifd_probe(struct platform_device *pdev)
{
    struct xocl_nifd *nifd;
    struct resource *res;
    struct xocl_dev_core *core;
    int err;

    printk("NIFD: probe => devm_kzalloc start");
    nifd = devm_kzalloc(&pdev->dev, sizeof(*nifd), GFP_KERNEL);
    printk("NIFD: probe => devm_kzalloc done");
    if (!nifd) {
        printk("NIFD: probe => devm_kzalloc err");
        return -ENOMEM;
    }
    // Map io memory to what was specified in the declaration
    printk("NIFD: probe => platform_get_resource start");
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    printk("NIFD: probe => platform_get_resource done, ioremap_nocache start");
    nifd->base_nifd = ioremap_nocache(res->start, res->end - res->start + 1);
    printk("NIFD: probe => ioremap_nocache done");

    if (!nifd->base_nifd)
    {
        printk("NIFD: probe => ioremap_nocache err");
        xocl_err(&pdev->dev, "Map iomem failed");
        return -EIO;
    }
    // Base NIFD should map to 0x28000
    // Base ICAP should map to 0x2c000

    // 5.2 DSA address
    nifd->base_icap = nifd->base_nifd + 0x4000;
    printk("NIFD: probe => xocl_get_xdev start");
    core = xocl_get_xdev(pdev);
    if (!core) {
        printk("NIFD: probe => core is null");
    }
    printk("NIFD: probe => xocl_get_xdev done");
    // Create the character device to access the ioctls
    printk("NIFD: probe => cdev_init start");
    cdev_init(&nifd->sys_cdev, &nifd_fops);
    printk("NIFD: probe => cdev_init done");
    nifd->sys_cdev.owner = THIS_MODULE;
    printk("NIFD: probe => nifd->sys_cdev.owner set");
    unsigned int device_id = XOCL_DEV_ID(core->pdev);
    printk("NIFD: probe => device_id set");
    unsigned int device_type = platform_get_device_id(pdev)->driver_data;
    printk("NIFD: probe => device_type set");
    nifd->instance =
        XOCL_DEV_ID(core->pdev) | platform_get_device_id(pdev)->driver_data;
    printk("NIFD: probe => nifd->instance set");
    nifd->sys_cdev.dev = MKDEV(MAJOR(nifd_dev), nifd->instance);
    printk("NIFD: probe => nifd->sys_cdev.dev set, cdev_add start");
    err = cdev_add(&nifd->sys_cdev, nifd->sys_cdev.dev, 1);
    printk("NIFD: probe => cdev_add done");
    if (err) {
        printk("NIFD: probe => cdev_add err");
        xocl_err(&pdev->dev, "NIFD cdev_add failed, %d", err);
        return err;
    }
    // Now create the system device to create the file
    printk("NIFD: probe => device_create start");
    nifd->sys_device = device_create(xrt_class, // was nifd_class
                                    &pdev->dev,
                                    nifd->sys_cdev.dev,
                                    NULL,
                                    "%s%d",
                                    platform_get_device_id(pdev)->name,
                                    nifd->instance);
    printk("NIFD: probe => device_create done");
    if (IS_ERR(nifd->sys_device)) {
        printk("NIFD: probe => device_create err");
        err = PTR_ERR(nifd->sys_device);
        cdev_del(&nifd->sys_cdev);
        return err;
    }
    printk("NIFD: probe => platform_set_drvdata start");
    platform_set_drvdata(pdev, nifd);
    printk("NIFD: probe => platform_set_drvdata done");
    return 0; // Success
}

static int nifd_remove(struct platform_device *pdev)
{
    struct xocl_dev_core *core;
    struct xocl_nifd *nifd;
    core = xocl_get_xdev(pdev);
    if (!core) {
        printk("NIFD: probe => core is null");
    }
    nifd = platform_get_drvdata(pdev);
    if (!nifd) {
        xocl_err(&pdev->dev, "driver data is NULL");
        return -EINVAL;
    }
    device_destroy(xrt_class, nifd->sys_cdev.dev);
    cdev_del(&nifd->sys_cdev);
    if (nifd->base_nifd) {
        iounmap(nifd->base_nifd);
    }
    platform_set_drvdata(pdev, NULL);
    devm_kfree(&pdev->dev, nifd);
    return 0; // Success
}

int __init xocl_init_nifd(void)
{
    int err = 0;
    printk("NIFD: alloc_chrdev_region start");
    err = alloc_chrdev_region(&nifd_dev, 0, 1, XOCL_NIFD);
    printk("NIFD: alloc_chrdev_region done");
    if (err < 0) {
        printk("NIFD: alloc_chrdev_region err");
        return err;
    }
    printk("NIFD: platform_driver_register start");
    err = platform_driver_register(&nifd_driver);
    printk("NIFD: platform_driver_register done");
    if (err) {
        printk("NIFD: platform_driver_register err");
        unregister_chrdev_region(nifd_dev, 1);
        return err;
    }
    printk("NIFD: init done");
    return 0; // Success
}

void xocl_fini_nifd(void)
{
    unregister_chrdev_region(nifd_dev, 1);
    platform_driver_unregister(&nifd_driver);
}