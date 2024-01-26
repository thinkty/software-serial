/*
 * 
 * Numerical 7-segment display device driver
 * 
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/kobject.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define START_BIT         (0)
#define STOP_BIT          (1)

#define DEVICE_NAME       "soft-serial"
#define DEVICE_WORKQUEUE  "soft-serial-workqueue"
#define DEVICE_FIFO_SIZE  (8) /* Size of the circular buffer */
#define DEVICE_TIMEOUT    (100) /* Time (ms) to wait until buffer has space */

/**
 * drvdata - driver data
 * 
 * @param workqueue Queue for tx_work and rx_work
 * @param tx_work Work that transmit each byte
 * @param tx_work_waitqueue Wait queue for tx_work
 * @param rx_work Work that receive each byte
 * @param rx_work_waitqueue TODO: not sure if needed yet
 *
 * @param tx_fifo Circular buffer for storing byte(s) to transmit
 * @param tx_fifo_mutex Mutex for the transmit buffer
 * @param has_tx_byte Whether there is a byte to transmit
 * @param byte Current byte to be transmitted via serial
 * @param writer_waitq Queue for writers when tx buffer is full
 *
 * @param closing Flag to stop work from self-queueing to workqueue
 * @param miscdev Used for getting drvdata using container_of
 * @param tx GPIO descriptor for transmission
 * @param rx GPIO descriptor for receival
 * @param tx_timer High resolution timer to handle bit-banging TODO: timer -> tx_timer
 * @param delay Nanoseconds before sending the next "bit" (computed by baudrate)
 */
struct drvdata {
    struct workqueue_struct * workqueue;
    struct work_struct tx_work;
    wait_queue_head_t tx_work_waitqueue;
    // struct work_struct rx_work;
    // wait_queue_head_t rx_work_waitqueue;

    struct kfifo tx_fifo;
    struct mutex tx_fifo_mutex;
    bool has_tx_byte;
    unsigned int tx_byte;
    wait_queue_head_t writer_waitq;

    bool closing;
    struct miscdevice miscdev;
    struct gpio_desc * tx;
    struct gpio_desc * rx;
    struct hrtimer tx_timer;
    ktime_t delay;
};

static int baudrate = 38400;
module_param(baudrate, int, 0);
MODULE_PARM_DESC(baudrate, "\tBaudrate of the device (default=38400)");

/**
 * Function prototypes for file operations
 */
static int release(struct inode * inode, struct file * filp);
static ssize_t write(struct file * filp, const char __user * buf, size_t count, loff_t * f_pos);

/**
 * File operations given as function pointers. .open is handled by default and
 * sets file->private_data to point to the structure.
 */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .release = release,
    // TODO: read
    .write = write,
};

/**
 * @brief Get the driver data from the given file.
 * 
 * @param filp Pointer to the open file for the device
 * 
 * @returns Device driver data
 */
static inline struct drvdata * get_drvdata(struct file * filp)
{
    struct miscdevice * dev = filp->private_data;
    return container_of(dev, struct drvdata, miscdev);
}

/**
 * @brief Release is called when the device file descriptor is closed. However,
 * deallocation of device related data structures is done on exit module.
 * 
 * @param inode Pointer to the file containing device metadata
 * @param filp Pointer to the open file for the device
 * 
 * @returns 0
 */
static int release(struct inode * inode, struct file * filp)
{
	unsigned int mn = iminor(inode);

    pr_info(DEVICE_NAME ": released %s%u\n", DEVICE_NAME, mn);
    return 0;
}

/**
 * @brief Check user input and write to the device buffer.
 * 
 * @param filp Pointer to the open file for the device
 * @param buf User-space buffer
 * @param count Number of bytes to write
 * @param f_pos Not used
 * 
 * @returns Number of bytes successfully written or a negative errno value.
 */
static ssize_t write(struct file * filp, const char __user * buf, size_t count, loff_t * f_pos)
{
    char tbuf[DEVICE_FIFO_SIZE] = {0};
    size_t to_copy = 0;
    size_t not_copied = 0;
    int err = 0;
    struct drvdata * dd = get_drvdata(filp);

    /* At maximum, the size of buffer */
    to_copy = count < DEVICE_FIFO_SIZE ? count : DEVICE_FIFO_SIZE;
    not_copied = copy_from_user(tbuf, buf, to_copy);
    to_copy -= not_copied;

    /* By being interruptible, when given any signal, the process will just
    give up on acquiring the lock and return -EINTR. */
    err = mutex_lock_interruptible(&dd->tx_fifo_mutex);
    if (err < 0) {
        return err;
    }

    /* Check in a loop since another writer may get to kfifo first. This could
    potentially be a thundering herd problem */
    while (kfifo_is_full(&dd->tx_fifo) && !dd->closing) {
        mutex_unlock(&dd->tx_fifo_mutex);

        /* If non-blocking, return immediately */
        if (filp->f_flags & O_NDELAY || filp->f_flags & O_NONBLOCK) {
            return -EAGAIN;
        }

        /* Sleep until space available, closing device, interrupt, or timeout */
        err = wait_event_interruptible_hrtimeout(dd->writer_waitq, dd->closing || !kfifo_is_full(&dd->tx_fifo), ms_to_ktime(DEVICE_TIMEOUT));
        if (err < 0) {
            /* Interrupted or timeout occurred */
            return err;
        }

        err = mutex_lock_interruptible(&dd->tx_fifo_mutex);
        if (err < 0) {
            return err;
        }
    }

    /* If closing device, return */
    if (dd->closing) {
        mutex_unlock(&dd->tx_fifo_mutex);
        return -ECANCELED;
    }

    /* Depending on the buffer vacancy, it might write less than specified */
    to_copy = kfifo_in(&dd->tx_fifo, tbuf, to_copy);
    mutex_unlock(&dd->tx_fifo_mutex);

    /* Wake up work waitqueue since there are bytes available to transmit */
    wake_up_interruptible(&dd->tx_work_waitqueue);

    return to_copy;
}

/**
 * @brief Callback function called by hrtimer to handle bit-banging. Since this
 * function runs in atomic context (interrupts disabled), do not sleep or
 * trigger a resched.
 * 
 * @param timer The hrtimer that called this callback function
 * 
 * @returns HRTIMER_RESTART until all the bits (start bit, data (8), and stop
 * bit (1)) are sent.
 */
static enum hrtimer_restart tx_timer_callback(struct hrtimer * timer)
{
    static unsigned int bit = -1;
    struct drvdata * dd = container_of(timer, struct drvdata, tx_timer);

    /* No byte to transmit, repeat timer */
    if (!dd->has_tx_byte) {
        hrtimer_forward_now(&dd->tx_timer, dd->delay);
        return HRTIMER_RESTART;
    }

    /* Start bit */
    if (bit == -1) {
        gpiod_set_value(dd->tx, START_BIT);
        bit++;
    }
    /* Data bits (8 bits) */
    else if (bit < 8) {
        gpiod_set_value(dd->tx, (dd->tx_byte & (1 << bit)) >> bit);
        bit++;
    }
    /* Stop bit */
    else {
        gpiod_set_value(dd->tx, STOP_BIT);
        bit = -1;
        dd->has_tx_byte = false;
        /* Signal to get next byte. Safe to call in atomic context */
        wake_up_interruptible(&dd->tx_work_waitqueue);
    }

    /* Restart the timer to handle next bit or byte */
    hrtimer_forward_now(&dd->tx_timer, dd->delay);
    return HRTIMER_RESTART;
}

/**
 * @brief Bottom half of the device driver to process available bytes in the
 * tx fifo buffer
 * 
 * @param work Pointer to the work structure
 */
static void tx_work_func(struct work_struct * work)
{
    int err;
    struct drvdata * dd = container_of(work, struct drvdata, tx_work);

    /* Sleep until there is something in fifo and the timer is ready to send
    another byte or the device is closing */
    err = wait_event_interruptible(dd->tx_work_waitqueue, dd->closing || (!kfifo_is_empty(&dd->tx_fifo) && !dd->has_tx_byte));
    if (err < 0 || dd->closing) {
        /* If interrupted or device is closing, stop the work */
        return;
    }

    /* Since there is only 1 actual device to write to, no locks here */
    if (kfifo_get(&dd->tx_fifo, &dd->tx_byte) == 0) {
        pr_warn(DEVICE_NAME ": kfifo_get(tx_fifo) no bytes available\n");
        queue_work(dd->workqueue, &dd->tx_work);
        return;
    }
    dd->has_tx_byte = true;

    /* Wake up writer_waitq since new space is available */
    wake_up_interruptible(&dd->writer_waitq);

    /* Self-requeueing work */
    queue_work(dd->workqueue, &dd->tx_work);
}

/**
 * @brief Initialize the device by allocating its numbers (major, minor),
 * create a misc (single character) device, and initialize the driver data.
 * 
 * @param pdev Platform device
 * 
 * @returns 0 on success, less than 0 on error.
 */
static int dt_probe(struct platform_device *pdev)
{
    int err = 0;
    struct drvdata * dd = NULL;

    /* Check that the device exists before attaching device driver */
    if (!device_property_present(&pdev->dev, "exists")) {
        pr_err(DEVICE_NAME ": device does not exist\n");
        return -ENODEV;
    }

    /* Allocate driver (zero-ed) data from kernel memory */
    dd = devm_kzalloc(&pdev->dev, sizeof(struct drvdata), GFP_KERNEL);
    if (!dd) {
        pr_err(DEVICE_NAME ": kzalloc() failed\n");
        return -ENOMEM;
    }

    /* TODO: Allocate tx & rx fifos */
    err = kfifo_alloc(&dd->tx_fifo, DEVICE_FIFO_SIZE, GFP_KERNEL);
    if (err != 0) {
        pr_err(DEVICE_NAME ": kfifo_alloc() failed\n");
        goto DT_PROBE_KFIFO_ALLOC;
    }

    /* Get the GPIO descriptors from the pin numbers */
    // TODO: rx as well
    // dd->rx = devm_gpiod_get_index(&pdev->dev, "serial", 1, GPIOD_IN);
    // if (IS_ERR(dd->rx)) {
    //     err = PTR_ERR(dd->rx);
    //     pr_err(DEVICE_NAME ": devm_gpiod_get(rx) failed\n");
    //     goto DT_PROBE_GPIOD_GET_RX;
    // }
    dd->tx = devm_gpiod_get_index(&pdev->dev, "serial", 0, GPIOD_OUT_HIGH);
    if (IS_ERR(dd->tx)) {
        err = PTR_ERR(dd->tx);
        pr_err(DEVICE_NAME ": devm_gpiod_get(tx) failed\n");
        goto DT_PROBE_GPIOD_GET_TX;
    }
    gpiod_set_value(dd->tx, 1); /* Set as stop bit */

    /* Initialize various wait queues and fifo mutexes */
    init_waitqueue_head(&dd->tx_work_waitqueue);
    init_waitqueue_head(&dd->writer_waitq);
    mutex_init(&dd->tx_fifo_mutex);
    // TODO: rx_fifo_mutex

    /* Create workqueue for the device to handle writing bytes */
    // TODO: workqueue for reading bytes
    dd->workqueue = alloc_ordered_workqueue(DEVICE_WORKQUEUE, 0);
    if (!dd->workqueue) {
        pr_err(DEVICE_NAME ": alloc_ordered_workqueue(tx) failed\n");
        err = -ENOMEM; /* However, there are multiple reasons to fail */
        goto DT_PROBE_ALLOC_WQ_TX;
    }

    /* Make the device available to the kernel & user */
    dd->miscdev.minor = MISC_DYNAMIC_MINOR;
    dd->miscdev.name = DEVICE_NAME;
    dd->miscdev.fops = &fops;
    dd->miscdev.mode = S_IRUGO | S_IWUGO;
    err = misc_register(&dd->miscdev);
    if (err < 0) {
        pr_err(DEVICE_NAME ": misc_register() failed\n");
        goto DT_PROBE_MISC_REG;
    }

    /* As the device is ready, queue work to start handling data if available */
    dd->closing = false;
    dd->has_tx_byte = false;
    dd->tx_byte = 0;
    INIT_WORK(&dd->tx_work, tx_work_func);
    queue_work(dd->workqueue, &dd->tx_work);

    /* Initialize and start the high resolution timer for TX */
    hrtimer_init(&dd->tx_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    dd->delay = ktime_set(0, NSEC_PER_SEC / baudrate);
    dd->tx_timer.function = tx_timer_callback;
    hrtimer_start(&dd->tx_timer, dd->delay, HRTIMER_MODE_REL);

    /* Set the driver data to the device */
    dev_set_drvdata(&pdev->dev, dd);

    pr_info(DEVICE_NAME ": successful init with baudrate=%d", baudrate);
    return 0;

DT_PROBE_MISC_REG:
    destroy_workqueue(dd->workqueue);
DT_PROBE_ALLOC_WQ_TX:
    mutex_destroy(&dd->tx_fifo_mutex);
DT_PROBE_GPIOD_GET_TX:
    kfifo_free(&dd->tx_fifo);
DT_PROBE_KFIFO_ALLOC:
    return err;
}

/**
 * @brief Free the device data, misc device, and the allocated device numbers.
 * 
 * @param pdev Platform device
 */
static int dt_remove(struct platform_device *pdev)
{
    struct drvdata * drv = dev_get_drvdata(&pdev->dev);
    if (!drv) {
        pr_err(DEVICE_NAME ": driver data does not exist\n");
        return -ENODATA;
    }

    /* Stop the bit-bang tx timer */
    hrtimer_cancel(&drv->tx_timer);

    /* Mark as closing so new work will not be added. */
    drv->closing = true;
    wake_up_interruptible(&drv->writer_waitq);
    wake_up_interruptible(&drv->tx_work_waitqueue);

    /* Unregister misc device */
    misc_deregister(&drv->miscdev);

    /* Cancel the remaining work, and wait for any remaining work */
    cancel_work_sync(&drv->tx_work);
    flush_workqueue(drv->workqueue); /* Just in case */
    destroy_workqueue(drv->workqueue);

    /* Cleanup driver data */
    mutex_destroy(&drv->tx_fifo_mutex);
    kfifo_free(&drv->tx_fifo);

    pr_info(DEVICE_NAME ": exit\n");
    return 0;
}

/**
 * Specifying the name of the device in device tree using the overlay
 */
static struct of_device_id dt_ids[] = {
    { .compatible = "thinkty,soft_serial", },
    { /* end of list */ },
};
MODULE_DEVICE_TABLE(of, dt_ids);

/**
 * N7D platform device operations given as function pointers
 */
static struct platform_driver software_serial_platform_driver = {
    .probe = dt_probe,
    .remove = dt_remove,
    .driver = {
        .name = DEVICE_NAME,
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(dt_ids),
    },
};

/* Macro for module init and exit */
module_platform_driver(software_serial_platform_driver);

MODULE_AUTHOR("Tae Yoon Kim");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Software serial device driver");
MODULE_VERSION("0:0.1");
