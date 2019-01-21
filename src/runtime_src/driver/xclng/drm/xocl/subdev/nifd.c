#define pr_fmt(fmt)     KBUILD_MODNAME ":%s: " fmt, __func__

#include "../xocl_drv.h"

static int nifd_open(struct inode *inode, struct file *file);

static int nifd_close(struct inode *inode, struct file *file);

static long nifd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

static int nifd_probe(struct platform_device *pdev);

static int nifd_remove(struct platform_device *pdev);

void xocl_fini_nifd(void);

int __init xocl_init_nifd(void);

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
    return 0;
}

static int nifd_remove(struct platform_device *pdev) {
    return 0;
}

int __init xocl_init_nifd(void) {
    return 0;
}

void xocl_fini_nifd(void)
{
    return;
}