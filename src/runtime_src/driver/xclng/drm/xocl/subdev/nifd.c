
#include "../xocl_drv.h"

// Offsets and definitions of all the hardware accessible registers in
//  the NIFD IP core.
enum NIFD_register_offset
{
    // A write-only register that turns the NIFD clock on in different modes.
    //  Writing 0 or 1 will turn it on in stepping mode.
    //  Writing 2 or 3 will turn it on in free running mode
    NIFD_START_APP = 0x0,

    // A write-only register that stops the NIFD clock when 1 is written to it
    NIFD_STOP_APP = 0x4,

    // A write-only register that clears all configuration memory.
    //  Write 1 to clear all memory.
    NIFD_CLEAR = 0x8,

    // A write-only register that clears configuration memory-1.
    //  Write 1 to clear the contents of Memory-1
    NIFD_CLEAR_CFG = 0xc,

    // A write-only register that clears the break status.
    //  Write 1 to this register to know that when we continue if the
    //  breakpoint signal goes high we have truly hit another breakpoint.
    NIFD_CLEAR_BREAKPOINT = 0x10,

    // A write-only register that configures the different modes that NIFD
    //  can run in.
    //  Bits[1:0] : Write 0 to specify stepping mode
    //  Bits[3:2] : Write 2 to operate NIFD for a specific number of clocks
    //            : Write 1 to operate NIFD until a breakpoint is hit
    //  Bits[5:4] : Write 0 to have readback data auto dequeued
    //            : Write 1 to have readback data dequeued by the host machine
    NIFD_CLK_MODES = 0x14,

    // A write-only register that starts a manual readback operation.
    //  If 0 is written, then we use memory-1.  If 1 is written,
    //  we use memory-2.
    NIFD_START_READBACK = 0x18,

    // A read/write register that specifies the number of clocks to run
    //  NIFD if configured to run for a set number of clocks
    NIFD_CLOCK_COUNT = 0x1c,

    // A write-only register that takes frame address, offsets, and constraints
    //  for Memory-1.
    NIFD_CONFIG_DATA = 0x20,

    // A write-only register that sets the final boolean equation using
    //  all of the sub-groups.
    NIFD_BREAKPOINT_CONDITION = 0x24,

    // A read-only register that returns the current status of the NIFD core.
    NIFD_STATUS = 0x28,

    // A read-only register that returns the current clock count during
    //  stepping.
    NIFD_CLOCK_CNT = 0x2c,

    // The register that we read in order to get data out.
    //  Does a read here read a single bit, or an entire word?
    NIFD_READBACK_DATA = 0x30,

    // The register that specifies how many words are present for reading the
    //  entire data out
    NIFD_READBACK_DATA_WORD_CNT = 0x34,

    // A write-only register that takes frame address, offsets, and constraints
    //  for Memory-2
    NIFD_CONFIG_DATA_M2 = 0x38,

    // A write-only register that clears the contents of Memory-2
    //  Write 1 to clear the contents of Memory-2
    NIFD_CLEAR_CFG_M2 = 0x3c
};

enum NIFD_COMMAND_SEQUENCES
{
    NIFD_ACQUIRE_CU = 0,
    NIFD_RELEASE_CU = 1,
    NIFD_QUERY_CU = 2,
    NIFD_READBACK_VARIABLE = 3,
    NIFD_SWITCH_ICAP_TO_NIFD = 4,
    NIFD_SWITCH_ICAP_TO_PR = 5,
    NIFD_ADD_BREAKPOINTS = 6,
    NIFD_REMOVE_BREAKPOINTS = 7,
    NIFD_CHECK_STATUS = 8,
    NIFD_QUERY_XCLBIN = 9,
    NIFD_STOP_CONTROLLED_CLOCK = 10,
    NIFD_START_CONTROLLED_CLOCK = 11,
    NIFD_SWITCH_CLOCK_MODE = 12
};

// Functions that get the current version of the driver and the IP
static int driver_version(void);
static int nifd_ip_version(void);
#define SUPPORTED_NIFD_IP_VERSION 1
#define SUPPORTED_DRIVER_VERSION 1

#define	MINOR_NAME_MASK		0xffffffff

// Low level functions to talk to NIFD and the ICAP
static void write_nifd_register(unsigned int value,
                                enum NIFD_register_offset reg_offset);
static unsigned int read_nifd_register(enum NIFD_register_offset reg_offset);
static void write_icap_mux_register(unsigned int value);
//static void         reset_icap_primitive(void);

// Helper functions that abstract some NIFD commands
static void start_controlled_clock_free_running(void);
static void start_controlled_clock_stepping(void);
static void restart_controlled_clock(unsigned int mode);
static void clear_configuration_memory(unsigned int bank);
static void add_breakpoint_data(unsigned int bank,
                                unsigned int frame,
                                unsigned int offset,
                                unsigned int constr);
static void add_readback_data(unsigned int frame,
                              unsigned int offset);
static unsigned int read_nifd_status(void);
static void perform_readback(unsigned int bank);

// Character device file operations
static int nifd_open(struct inode *inode, struct file *file);
static int nifd_close(struct inode *inode, struct file *file);
static long nifd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

// Different Ioctl commands
static long switch_clock_mode(void __user *arg);
static long acquire_cu(void);
static long release_cu(void);
static long query_cu(void);
static long readback_variable(void __user *arg);
static long switch_icap_to_nifd(void);
static long switch_icap_to_pr(void);
static long add_breakpoints(void __user *arg);
static long remove_breakpoints(void);
static long check_status(void __user *arg);
static long query_xclbin(void);
static void stop_controlled_clock(void);
static long start_controlled_clock(void __user *arg);

// Ioctl helper functions
static long readback_variable_core(unsigned int *arg);
static long add_breakpoints_core(unsigned int *arg);

// Platform driver commands for subdevices
static int nifd_probe(struct platform_device *pdev);
static int nifd_remove(struct platform_device *pdev);

// Initialization and finalization functions
int __init xocl_init_nifd(void);
void xocl_fini_nifd(void);

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

// Global variables
static dev_t nifd_dev;
// static struct class *nifd_class = NULL;
struct xocl_nifd *nifd_global = NULL;

// ---------------------------------------------
// Versioning functions
// ---------------------------------------------
static int driver_version(void)
{
    return 1;
}

static int nifd_ip_version(void)
{
    return 1;
}

// ---------------------------------------------
// Low level helper functions to talk to NIFD
// ---------------------------------------------
static void write_nifd_register(unsigned int value,
                                enum NIFD_register_offset reg_offset)
{
    unsigned int offset_value = (unsigned int)(reg_offset);
    unsigned long long int full_addr =
        (unsigned long long int)(nifd_global->base_nifd) + offset_value;
    void *ptr = (void *)(full_addr);

    iowrite32(value, ptr);
}

static unsigned int read_nifd_register(enum NIFD_register_offset reg_offset)
{
    unsigned int offset_value = (unsigned int)(reg_offset);
    unsigned long long int full_addr =
        (unsigned long long int)(nifd_global->base_nifd) + offset_value;
    void *ptr = (void *)(full_addr);

    return ioread32(ptr);
}

static void write_icap_mux_register(unsigned int value)
{
    iowrite32(value, nifd_global->base_icap);
}

//static void reset_icap_primitive()
//{
// Force the ICAP primitive to reset by writing to its status register
//  unsigned long long int primitiveStatusRegister =
//    (unsigned long long int)(nifd_global->base_icap_primitive) + 0x10c ;
//
//  iowrite32(0x40, (void*)(primitiveStatusRegister)) ;
//}

// -------------------------------------------------------
// Local helper functions that abstract some NIFD commands
// -------------------------------------------------------
static void start_controlled_clock_free_running(void)
{
    write_nifd_register(0x3, NIFD_START_APP);
}

static void start_controlled_clock_stepping(void)
{
    write_nifd_register(0x0, NIFD_START_APP);
}

static void restart_controlled_clock(unsigned int previousMode)
{
    if (previousMode == 0x1)
        start_controlled_clock_free_running();
    else if (previousMode == 0x2)
        start_controlled_clock_stepping();
}

static void clear_configuration_memory(unsigned int bank)
{
    switch (bank)
    {
    case 1:
        write_nifd_register(0x1, NIFD_CLEAR_CFG);
        break;
    case 2:
        write_nifd_register(0x1, NIFD_CLEAR_CFG_M2);
        break;
    default:
        // Clear both memories
        write_nifd_register(0x1, NIFD_CLEAR);
        break;
    }
}

static void add_breakpoint_data(unsigned int bank,
                                unsigned int frame,
                                unsigned int offset,
                                unsigned int constraint)
{
    enum NIFD_register_offset register_offset;
    switch (bank)
    {
    case 1:
        register_offset = NIFD_CONFIG_DATA;
        break;
    case 2:
        register_offset = NIFD_CONFIG_DATA_M2;
        break;
    default:
        // Do not assign to either bank
        return;
    }

    frame &= 0x3fffffff;      // Top two bits of frames must be 00
    offset &= 0x3fffffff;     // Set the top two bits to 0 first
    constraint &= 0x3fffffff; // Set the top two bits to 0 first
    offset |= 0x80000000;     // Top two bits of offsets must be 10
    constraint |= 0x40000000; // Top two bits of constraints must be 01

    write_nifd_register(frame, register_offset);

    // Switching to match Mahesh's test
    write_nifd_register(constraint, register_offset);
    write_nifd_register(offset, register_offset);
}

static void add_readback_data(unsigned int frame, unsigned int offset)
{
    frame &= 0x3fffffff;  // Top two bits of frames must be 00
    offset &= 0x3fffffff; // Set the top two bits to 0 first
    offset |= 0x80000000; // Top two bits of offsets must be 10

    //printk("NIFD: Frame: %x Offset: %x\n", frame, offset);

    write_nifd_register(frame, NIFD_CONFIG_DATA_M2);
    write_nifd_register(offset, NIFD_CONFIG_DATA_M2);
}

static unsigned int read_nifd_status(void)
{
    return read_nifd_register(NIFD_STATUS);
}

static void perform_readback(unsigned int bank)
{
    unsigned int commandWord;
    if (bank == 1)
    {
        commandWord = 0x0;
    }
    else if (bank == 2)
    {
        commandWord = 0x1;
    }
    else
    {
        return;
    }

    write_nifd_register(commandWord, NIFD_START_READBACK);
}

// ---------------------------------------------
// Character device file operations
// ---------------------------------------------
static int nifd_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int nifd_close(struct inode *inode, struct file *file)
{
    return 0;
}

static long nifd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    void __user *data = (void __user *)(arg);

    // The current driver is version 1 and works on version 1 of NIFD IP
    if (driver_version() > SUPPORTED_DRIVER_VERSION ||
        nifd_ip_version() > SUPPORTED_NIFD_IP_VERSION)
    {
        return -EINVAL;
    }

    switch (cmd)
    {
    case NIFD_SWITCH_CLOCK_MODE:
        return switch_clock_mode(data);
    case NIFD_ACQUIRE_CU:
        return acquire_cu();
    case NIFD_RELEASE_CU:
        return release_cu();
    case NIFD_QUERY_CU:
        return query_cu();
    case NIFD_READBACK_VARIABLE:
        return readback_variable(data);
    case NIFD_SWITCH_ICAP_TO_NIFD:
        return switch_icap_to_nifd();
    case NIFD_SWITCH_ICAP_TO_PR:
        return switch_icap_to_pr();
    case NIFD_ADD_BREAKPOINTS:
        return add_breakpoints(data);
    case NIFD_REMOVE_BREAKPOINTS:
        return remove_breakpoints();
    case NIFD_CHECK_STATUS:
        return check_status(data);
    case NIFD_QUERY_XCLBIN:
        return query_xclbin();
    case NIFD_STOP_CONTROLLED_CLOCK:
        stop_controlled_clock();
        return 0;
    case NIFD_START_CONTROLLED_CLOCK:
        return start_controlled_clock(data);
    default:
        break;
    }

    return -EINVAL;
}

// ---------------------------------------------
// Ioctl sub-commands
// ---------------------------------------------

static long switch_clock_mode(void __user *arg)
{
    // Currently making sure the clock is in the correct mode to do breakpointing
    write_nifd_register(0x04, NIFD_CLK_MODES);
    return 0;
}

static long acquire_cu(void)
{
    return 0;
}

static long release_cu(void)
{
    return 0;
}

static long query_cu(void)
{
    return 0;
}

static long readback_variable(void __user *arg)
{
    // Allocate memory in kernel space, copy over all the information
    //  from user space at once, call the core implemenation,
    //  and finally write back the result.

    // The information will be passed in this format:
    //  [numBits][frame][offset][frame][offset]...[space for result]
    unsigned int num_bits;
    unsigned int num_words;
    unsigned int total_data_size;
    unsigned int *kernel_memory;
    unsigned int core_result;

    if (copy_from_user(&num_bits, arg, sizeof(unsigned int)))
        return -EFAULT;

    num_words = num_bits % 32 ? num_bits / 32 + 1 : num_bits / 32;

    total_data_size = (1 + (num_bits * 2) + num_words) * sizeof(unsigned int);

    //total_data_size = (num_bits * 3 + 1) * sizeof(unsigned int) ;
    kernel_memory = (unsigned int *)(kmalloc(total_data_size, GFP_KERNEL));

    if (!kernel_memory)
        return -ENOMEM;

    if (copy_from_user(kernel_memory, arg, total_data_size))
    {
        kfree(kernel_memory);
        return -EFAULT;
    }

    core_result = readback_variable_core(kernel_memory);

    if (core_result)
    {
        kfree(kernel_memory);
        return core_result;
    }

    if (copy_to_user(arg, kernel_memory, total_data_size))
    {
        kfree(kernel_memory);
        return -EFAULT;
    }

    kfree(kernel_memory);
    return 0; // Success
}

static long switch_icap_to_nifd(void)
{
    write_icap_mux_register(0x1);
    return 0;
}

static long switch_icap_to_pr(void)
{
    write_icap_mux_register(0x0);
    //reset_icap_primitive() ;
    return 0;
}

static long add_breakpoints(void __user *arg)
{
    // Format of user data:
    //  [numBreakpoints][frameAddress][frameOffset][constraint]...[condition]
    unsigned int num_breakpoints;
    unsigned int total_data_size;
    unsigned int *kernel_memory;
    long result;

    if (copy_from_user(&num_breakpoints, arg, sizeof(unsigned int)))
        return -EFAULT;

    total_data_size = (num_breakpoints * 3 + 1 + 1) * sizeof(unsigned int);
    kernel_memory = (unsigned int *)(kmalloc(total_data_size, GFP_KERNEL));
    if (!kernel_memory)
        return -ENOMEM;

    if (copy_from_user(kernel_memory, arg, total_data_size))
    {
        kfree(kernel_memory);
        return -EFAULT;
    }

    result = add_breakpoints_core(kernel_memory);

    // I don't need to copy anything back to user memory
    kfree(kernel_memory);

    if (result)
        return result; // Failure
    return 0;          // Success
}

static long remove_breakpoints(void)
{
    unsigned int clock_status = (read_nifd_status() & 0x3);

    stop_controlled_clock();
    clear_configuration_memory(0);
    //write_nifd_register(0x1, NIFD_CLEAR_BREAKPOINT) ;
    write_nifd_register(0x1, NIFD_CLEAR);
    restart_controlled_clock(clock_status);

    return 0;
}

static long check_status(void __user *arg)
{
    unsigned int status = read_nifd_status();

    if (copy_to_user(arg, &status, sizeof(unsigned int)))
    {
        return -EFAULT;
    }

    return 0; // Success
}

static long query_xclbin(void)
{
    return 0;
}

static void stop_controlled_clock(void)
{
    write_nifd_register(0x1, NIFD_STOP_APP);
}

static long start_controlled_clock(void __user *arg)
{
    unsigned int mode = 0;
    if (copy_from_user(&mode, arg, sizeof(unsigned int)))
    {
        return -EFAULT;
    }
    restart_controlled_clock(mode);
    if (mode == 1 || mode == 2)
        return 0;
    return -EINVAL; // Improper input
}

// ---------------------------------------------
// Ioctl helper functions
// ---------------------------------------------

static long readback_variable_core(unsigned int *arg)
{
    // This function performs the readback operation.  The argument
    //  input data and the result storage is completely located
    //  in kernel space.

    unsigned int clock_status;
    unsigned int num_bits;
    unsigned int i;
    unsigned int frame;
    unsigned int offset;
    unsigned int next_word = 0;
    unsigned int readback_status = 0;
    unsigned int readback_data_word_cnt = 0;

    // Check the current status of the clock and record if it is running
    clock_status = (read_nifd_status() & 0x3);

    // If the clock was running in free running mode, we have to
    //  put it into stepping mode for a little bit in order to get
    //  this to work.  This is a bug in the hardware that needs to be fixed.
    if (clock_status == 1)
    {
        stop_controlled_clock();
        start_controlled_clock_stepping();
    }

    // Stop the clock no matter what
    stop_controlled_clock();

    // Clear Memory-2
    clear_configuration_memory(2);

    // Fill up Memory-2 with all the frames and offsets passed in.
    //  The data is passed in the format of:
    //  [num_bits][frame][offset][frame][offset]...[space for result]
    num_bits = *arg;
    ++arg;
    for (i = 0; i < num_bits; ++i)
    {
        frame = *arg;
        ++arg;
        offset = *arg;
        ++arg;
        add_readback_data(frame, offset);
    }

    perform_readback(2);

    // I should be reading 32-bit words at a time
    readback_status = 0;
    while (readback_status == 0)
    {
        readback_status = (read_nifd_status() & 0x8);
    }

    // The readback is ready, so we need to figure out how many words to read
    readback_data_word_cnt = read_nifd_register(NIFD_READBACK_DATA_WORD_CNT);

    for (i = 0; i < readback_data_word_cnt; ++i)
    {
        next_word = read_nifd_register(NIFD_READBACK_DATA);
        (*arg) = next_word;
        ++arg;
    }

    restart_controlled_clock(clock_status);

    //printk("NIFD: Final status: %x\n", read_nifd_status());
    return 0; // Success
}

static long add_breakpoints_core(unsigned int *arg)
{
    // Format of user data:
    //  [numBreakpoints][frameAddress][frameOffset][constraint]...[condition]

    unsigned int num_breakpoints;
    unsigned int i;
    unsigned int frame_address;
    unsigned int frame_offset;
    unsigned int constraint;
    unsigned int breakpoint_condition;

    // When adding breakpoints, the clock should be stopped
    unsigned int clock_status = (read_nifd_status() & 0x3);
    if (clock_status != 0x3)
        return -EINVAL;

    // All breakpoints need to be set at the same time
    clear_configuration_memory(1);

    num_breakpoints = (*arg);

    ++arg;

    for (i = 0; i < num_breakpoints; ++i)
    {
        frame_address = (*arg);
        ++arg;
        frame_offset = (*arg);
        ++arg;
        constraint = (*arg);
        ++arg;

        add_breakpoint_data(1, frame_address, frame_offset, constraint);
    }

    breakpoint_condition = (*arg);

    write_nifd_register(breakpoint_condition, NIFD_BREAKPOINT_CONDITION);

    return 0; // Success
}

// ---------------------------------------------
// Platform driver commands for subdevices
// ---------------------------------------------
static int nifd_probe(struct platform_device *pdev)
{
    struct xocl_nifd *nifd;
    struct resource *res;
    struct xocl_dev_core *core;
    int err;

    printk("NIFD: nifd structs constructed");

    //xocl_info(&pdev->dev, "Starting NIFD probe\n") ;

    printk("NIFD: allocating vm");
    nifd = devm_kzalloc(&pdev->dev, sizeof(*nifd), GFP_KERNEL);
    printk("NIFD: vm allocated");

    if (!nifd)
        return -ENOMEM;
    nifd_global = nifd;

    // Map io memory to what was specified in the declaration
    printk("NIFD: getting platform resource");
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    printk("NIFD: platform resource got");

    printk("NIFD: start ioremap_nocache");
    nifd->base_nifd = ioremap_nocache(res->start, res->end - res->start + 1);
    printk("NIFD: ioremap_nocache return");

    if (!nifd->base_nifd)
    {
        printk("NIFD: ioremap_nocache err");
        xocl_err(&pdev->dev, "Map iomem failed");
        return -EIO;
    }
    // Base NIFD should map to 0x28000
    // Base ICAP should map to 0x2c000

    // 5.2 DSA address
    printk("NIFD: get base_icap");
    nifd->base_icap = nifd->base_nifd + 0x4000;
    printk("NIFD: get base_icap return");

    // The location of the ICAP primitive on 5.2 is at 0x20000
    //nifd->base_icap_primitive = ioremap_nocache(0x20000, 0x20119) ;
    //if (!nifd->base_icap_primitive)
    //{
    //  xocl_err(&pdev->dev, "Map primitive failed");
    // This appears to be failing...
    //}

    printk("NIFD: start xocl_get_xdev");
    core = xocl_get_xdev(pdev);
    printk("NIFD: xocl_get_xdev return");

    // Create the character device to access the ioctls
    printk("NIFD: cdev_init start");
    cdev_init(&nifd->sys_cdev, &nifd_fops);
    printk("NIFD: cdev_init return");

    printk("NIFD: sys_cdev start");
    nifd->sys_cdev.owner = THIS_MODULE;
    nifd->instance =
        XOCL_DEV_ID(core->pdev) | platform_get_device_id(pdev)->driver_data;
    nifd->sys_cdev.dev = MKDEV(MAJOR(nifd_dev), nifd->instance);
    printk("NIFD: sys_cdev return");

    printk("NIFD: cdev_add start");
    err = cdev_add(&nifd->sys_cdev, nifd->sys_cdev.dev, 1);
    printk("NIFD: cdev_add return");

    if (err)
    {
        xocl_err(&pdev->dev, "NIFD cdev_add failed, %d", err);
        return err;
    }

    // Now create the system device to create the file
    printk("NIFD: device_create start");
    nifd->sys_device = device_create(xrt_class, // was nifd_class
                                     &pdev->dev,
                                     nifd->sys_cdev.dev,
                                     NULL,
                                     "%s%d",
                                     platform_get_device_id(pdev)->name,
                                     nifd->instance);
    printk("NIFD: device_create return");

    if (IS_ERR(nifd->sys_device))
    {
        err = PTR_ERR(nifd->sys_device);
        cdev_del(&nifd->sys_cdev);
        return err;
    }

    printk("NIFD: platform_set_drvdata start");
    platform_set_drvdata(pdev, nifd);
    printk("NIFD: platform_set_drvdata return");

    return 0; // Success
}

static int nifd_remove(struct platform_device *pdev)
{
    struct xocl_nifd *nifd;

    printk("NIFD: remove platform_get_drvdata start");
    nifd = platform_get_drvdata(pdev);
    printk("NIFD: platform_get_drvdata return");

    if (!nifd) {
        printk("NIFD: platform_get_drvdata err");
        return -EINVAL;
    }

    if (!xrt_class) {
        printk("NIFD: xrt_class is NULL");
        return -EINVAL;
    }

    if (!nifd->sys_cdev.dev) {
        printk("NIFD: sys_cdev.dev is NULL");
        return -EINVAL;
    }
        
    printk("NIFD: device_destroy start");
    // device_destroy(nifd_class, nifd->sys_cdev.dev);
    device_destroy(xrt_class, nifd->sys_cdev.dev);
    printk("NIFD: device_destroy return");

    printk("NIFD: cdev_del start");
    cdev_del(&nifd->sys_cdev);
    printk("NIFD: cdev_del return");

    if (nifd->base_nifd) {
        printk("NIFD: cdev_del err");
        iounmap(nifd->base_nifd);
    }

    printk("NIFD: platform_set_drvdata start");
    platform_set_drvdata(pdev, NULL);
    printk("NIFD: platform_set_drvdata return");

    printk("NIFD: devm_kfree start");
    devm_kfree(&pdev->dev, nifd);
    printk("NIFD: devm_kfree return");

    nifd_global = NULL;

    printk("NIFD: remove all done");

    return 0; // Success
}

// ---------------------------------------------
// Initialization and finalization functions
// ---------------------------------------------
int __init xocl_init_nifd(void)
{
    int err = 0;
    printk("NIFD: init nifd");

    printk("NIFD: alloc_chrdev_region start");
    err = alloc_chrdev_region(&nifd_dev, 0, 1, XOCL_NIFD);
    printk("NIFD: alloc_chrdev_region return");

    if (err < 0)
    {
        printk("NIFD: alloc_chrdev_region err");
        return err;
    }
    
    /*
    printk("NIFD: class_create start");
    nifd_class = class_create(THIS_MODULE, XOCL_NIFD);
    printk("NIFD: class_create return");

    if (!nifd_class) {
        printk("NIFD: nifd_class is NULL");
    }

    if (IS_ERR(nifd_class))
    {
        printk("NIFD: class_create err");
        err = PTR_ERR(nifd_class);
        unregister_chrdev_region(nifd_dev, 1);
        return err;
    }
    */

    printk("NIFD: platform_driver_register start");
    err = platform_driver_register(&nifd_driver);
    printk("NIFD: platform_driver_register return");

    if (err)
    {
        printk("NIFD: platform_driver_register err");
        // class_destroy(nifd_class);
        class_destroy(xrt_class);
        unregister_chrdev_region(nifd_dev, 1);
        return err;
    }

    printk("NIFD: all done");
    return 0; // Success
}

void xocl_fini_nifd(void)
{
    unregister_chrdev_region(nifd_dev, 1);
    // class_destroy(nifd_class);
    // class_destroy(xrt_class);
    platform_driver_unregister(&nifd_driver);
}
