/*
 * 
 * Numerical 7-segment display device driver
 * 
 */

#include "linux/irqreturn.h"
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/interrupt.h>
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

#define DEVICE_NAME       "soft_serial"
#define DEVICE_WORKQUEUE  "soft_serial-workqueue"
#define DEVICE_FIFO_SIZE  (64)  /* Size of the circular buffer */
#define DEVICE_TIMEOUT    (100) /* Time (ms) to wait until buffer has space */

/**
 * drvdata - device data TODO: clean up this stuff
 *
 * @param workqueue Queue for tx_work and rx_work
 * @param tx_work Work that transmit each byte
 * @param tx_work_waitqueue Wait queue for tx_work
 * @param rx_work Work that receive each byte
 * @param rx_work_waitqueue TODO: not sure if needed yet
 *
 * @param tx_fifo Circular buffer for storing byte(s) to transmit
 * @param tx_fifo_mutex Mutex for the transmit buffer
 * @param writer_waitq Queue for writers when tx buffer is full
 *
 * @param rx_irq IRQ number for getting the interrupts of RX line 
 *
 * @param closing Flag to stop work from self-queueing to workqueue
 * @param miscdev Used for getting drvdata using container_of
 * @param tx_gpio GPIO descriptor for transmission
 * @param rx_gpio GPIO descriptor for receival
 * @param tx_timer High resolution timer to handle bit-banging
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
    wait_queue_head_t writer_waitq;

    int rx_irq;

    bool closing;
    struct miscdevice miscdev;
    struct gpio_desc * tx_gpio;
    struct gpio_desc * rx_gpio;
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
    // TODO: open : request irq if not already done. keep count of open so that irq can be released in last release
    .release = release, // TODO: release irq if last device (using count of open)
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

// TODO: open

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
 * @brief Write to the device buffer (fifo). Since there can be simultaneous
 * access to write, a mutex lock is used. If the buffer is full, the write
 * operation hangs until a wake up is received from the work function. If the
 * user specified non-blocking, it will return as soon as it is blockde.
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

    // TODO: no need to use work function
    // /* If the clock isn't waiting or executing handler, start the timer. */
    // if (!hrtimer_active(&dd->tx_timer)) {
    //     hrtimer_start(&dd->tx_timer, dd->delay, HRTIMER_MODE_REL);
    // }

    return to_copy;
}

/**
 * @brief Bottom half of the device driver to process available bytes in the tx
 * fifo buffer. There are two waits in the work function. First wait is for the
 * bytes to send, and the second wait is for the timer to be ready to send the
 * next bytes. In both waits, it also checks if the device is closing. When the
 * device is closing, it does not self-requeue.
 *
 * TODO: is work function not needed? can we just directly start hrtimer in write? (start if not active as starting removes the timer). As kfifo_in increments in after inserting, timer function does not need to worry about partial insert.
 * 
 * @param work Pointer to the work structure
 */
static void tx_work_func(struct work_struct * work)
{
    int err;
    struct drvdata * dd = container_of(work, struct drvdata, tx_work);

    /* Sleep until there is something in tx_fifo or the device is closing */
    err = wait_event_interruptible(dd->tx_work_waitqueue, dd->closing || !kfifo_is_empty(&dd->tx_fifo));
    if (err < 0 || dd->closing) {
        /* If interrupted or device is closing, stop the work */
        return;
    }

    /* Sleep until the hrtimer is ready to send or the device is closing */
    err = wait_event_interruptible(dd->tx_work_waitqueue, dd->closing || !hrtimer_active(&dd->tx_timer));
    if (err < 0 || dd->closing) {
        return;
    }

    hrtimer_start(&dd->tx_timer, dd->delay, HRTIMER_MODE_REL);

    /* Sleep until the hrtimer has sent one or more bytes */
    err = wait_event_interruptible(dd->tx_work_waitqueue, dd->closing || !hrtimer_active(&dd->tx_timer));
    if (err < 0 || dd->closing) {
        return;
    }

    /* Wake up writer_waitq in case it was waiting for space in fifo */
    wake_up_interruptible(&dd->writer_waitq);

    /* Self-requeueing work */
    queue_work(dd->workqueue, &dd->tx_work);
    return;
}

/**
 * @brief Callback function called by hrtimer to handle bit-banging. Since this
 * function runs in interrupt context, do not sleep or trigger a resched. The
 * timer is stopped after transmitting the last bit. wake_up() and kfifo_get()
 * are safe to call in interrupt context. The function transmits until there are
 * no bytes left in the fifo.
 * 
 * @param timer The hrtimer that called this callback function
 * 
 * @returns HRTIMER_RESTART until all the bits (start bit, data (8), and stop
 * bit (1)) are sent.
 */
static enum hrtimer_restart tx_timer_callback(struct hrtimer * timer)
{
    static unsigned int bit = -1;
    static unsigned char byte = 0;
    struct drvdata * dd = container_of(timer, struct drvdata, tx_timer);

    /* Start bit */
    if (bit == -1) {
        /* If nothing was retrieved, wake up the work func for more. */
        /* Since there is only 1 actual device to write to, no locks here */
        if (kfifo_get(&dd->tx_fifo, &byte) == 0) {
            wake_up_interruptible(&dd->tx_work_waitqueue);
            return HRTIMER_NORESTART;
        }

        gpiod_set_value(dd->tx_gpio, START_BIT);
        bit++;
    }
    /* Data bits (8 bits) */
    else if (bit < 8) {
        gpiod_set_value(dd->tx_gpio, (byte & (1 << bit)) >> bit);
        bit++;
    }
    /* Stop bit */
    else {
        gpiod_set_value(dd->tx_gpio, STOP_BIT);
        bit = -1;
        byte = 0;
    }

    /* Restart timer to handle next bit */
    hrtimer_forward_now(timer, dd->delay);
    return HRTIMER_RESTART;
}

// TODO: read

/**
 * @brief Interrupt service routine for the RX line interrupts. TODO:
 *
 * @param irq 
 * @param data 
 *
 * @returns IRQ_HANDLED. 
 */
static irqreturn_t rx_isr(int irq, void * data)
{
    struct platform_device * pdev = (struct platform_device *) data;
    struct drvdata * dd = (struct drvdata *) dev_get_drvdata(&pdev->dev);
    pr_info(DEVICE_NAME ": interrupted on irq=%d (saved irq=%d)\n", irq,  dd->rx_irq);
    return IRQ_HANDLED;
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
    int ret = 0;
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
    dev_set_drvdata(&pdev->dev, dd);

    /* TODO: Allocate tx & rx fifos */
    ret = kfifo_alloc(&dd->tx_fifo, DEVICE_FIFO_SIZE, GFP_KERNEL);
    if (ret != 0) {
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
    dd->tx_gpio = devm_gpiod_get_index(&pdev->dev, "serial", 0, GPIOD_OUT_HIGH);
    if (IS_ERR(dd->tx_gpio)) {
        ret = PTR_ERR(dd->tx_gpio);
        pr_err(DEVICE_NAME ": devm_gpiod_get(tx) failed\n");
        goto DT_PROBE_GPIOD_GET_TX;
    }
    gpiod_set_value(dd->tx_gpio, 1); /* Set as stop bit */

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
        ret = -ENOMEM; /* However, there are multiple reasons to fail */
        goto DT_PROBE_ALLOC_WQ_TX;
    }

    /* As the device is ready, queue work to start handling data if available */
    dd->closing = false;
    INIT_WORK(&dd->tx_work, tx_work_func);
    queue_work(dd->workqueue, &dd->tx_work);

    /* Initialize the high resolution timer for TX */
    hrtimer_init(&dd->tx_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    dd->delay = ktime_set(0, NSEC_PER_SEC / baudrate);
    dd->tx_timer.function = tx_timer_callback;

    /* Get IRQ from platform device (device tree) and request IRQ */
    dd->rx_irq = platform_get_irq(pdev, 0);
    if (dd->rx_irq < 0) {
        ret = -ENODATA;
        pr_err(DEVICE_NAME ": platform_get_irq() failed\n");
        goto DT_PROBE_REQUEST_IRQ;
    }
    ret = devm_request_irq(&pdev->dev, dd->rx_irq, rx_isr, IRQF_TRIGGER_NONE, dev_name(&pdev->dev), pdev);
    if (ret < 0) {
        pr_err(DEVICE_NAME ": devm_request_irq() failed\n");
        goto DT_PROBE_REQUEST_IRQ;
    }

    /* Make the device available to the kernel & user */
    dd->miscdev.minor = MISC_DYNAMIC_MINOR;
    dd->miscdev.name = DEVICE_NAME;
    dd->miscdev.fops = &fops;
    dd->miscdev.mode = S_IRUGO | S_IWUGO;
    ret = misc_register(&dd->miscdev);
    if (ret < 0) {
        pr_err(DEVICE_NAME ": misc_register() failed\n");
        goto DT_PROBE_MISC_REG;
    }

    pr_info(DEVICE_NAME ": successful init with baudrate=%d", baudrate);
    return 0;

DT_PROBE_MISC_REG:
DT_PROBE_REQUEST_IRQ:
    hrtimer_cancel(&dd->tx_timer);
    dd->closing = true;
    wake_up_interruptible(&dd->writer_waitq);
    wake_up_interruptible(&dd->tx_work_waitqueue);
    misc_deregister(&dd->miscdev);
    cancel_work_sync(&dd->tx_work);
    flush_workqueue(dd->workqueue);
    destroy_workqueue(dd->workqueue);
DT_PROBE_ALLOC_WQ_TX:
    mutex_destroy(&dd->tx_fifo_mutex);
DT_PROBE_GPIOD_GET_TX:
    kfifo_free(&dd->tx_fifo);
DT_PROBE_KFIFO_ALLOC:
    return ret;
}

/**
 * @brief Free the device data, misc device, and the allocated device numbers.
 * 
 * @param pdev Platform device
 */
static int dt_remove(struct platform_device *pdev)
{
    struct drvdata * dd = dev_get_drvdata(&pdev->dev);
    if (!dd) {
        pr_err(DEVICE_NAME ": driver data does not exist\n");
        return -ENODATA;
    }

    /* Unregister misc device */
    misc_deregister(&dd->miscdev);

    /* Stop the bit-bang tx timer */
    hrtimer_cancel(&dd->tx_timer);

    /* Mark as closing so new work will not be added. */
    dd->closing = true;
    wake_up_interruptible(&dd->writer_waitq);
    wake_up_interruptible(&dd->tx_work_waitqueue);

    /* Cancel the remaining work, and wait for any remaining work */
    cancel_work_sync(&dd->tx_work);
    flush_workqueue(dd->workqueue); /* Just in case */
    destroy_workqueue(dd->workqueue);

    /* Cleanup driver data */
    mutex_destroy(&dd->tx_fifo_mutex);
    kfifo_free(&dd->tx_fifo);

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
static struct platform_driver soft_serial_platform_driver = {
    .probe = dt_probe,
    .remove = dt_remove,
    .driver = {
        .name = DEVICE_NAME,
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(dt_ids),
    },
};

/* Macro for module init and exit */
module_platform_driver(soft_serial_platform_driver);

MODULE_AUTHOR("Tae Yoon Kim");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Software serial device driver");
MODULE_VERSION("0:0.1");
