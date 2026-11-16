#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/serdev.h>
#include <linux/cdev.h>
#include <linux/kfifo.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "ch9437_dev.h"

#define CHDEV_RX_FIFO_SIZE	8192
#define CHDEV_MAX_DEVS		8
#define CHDEV_MAX_WRITE		4096

struct chdev_priv {
	struct serdev_device *serdev;
	struct device *chrdev;
	struct cdev cdev;
	dev_t devt;
	int ch;
	unsigned int baud;
	bool opened;
	char name[32];
	int name_slot;

	/* rx: single producer (ch9437 worker) / single consumer (read) */
	struct kfifo rx_fifo;
	wait_queue_head_t rwait;

	struct mutex write_lock;

	atomic_long_t rx_bytes;
	atomic_long_t tx_bytes;
	atomic_long_t overruns;
};

static dev_t chdev_devt;
static struct class *chdev_class;

/* device node name registry, duplicates get a _<ch> suffix */
static DEFINE_MUTEX(chdev_names_lock);
static char chdev_names[CHDEV_MAX_DEVS][32];

static int chdev_receive_buf(struct serdev_device *serdev,
			     const unsigned char *buf, size_t count)
{
	struct chdev_priv *priv = serdev_device_get_drvdata(serdev);
	unsigned int n;

	n = kfifo_in(&priv->rx_fifo, buf, count);
	if (n < count)
		atomic_long_add(count - n, &priv->overruns);
	atomic_long_add(n, &priv->rx_bytes);

	wake_up_interruptible(&priv->rwait);
	return n;
}

static void chdev_write_wakeup(struct serdev_device *serdev)
{
}

static const struct serdev_device_ops chdev_serdev_ops = {
	.receive_buf = chdev_receive_buf,
	.write_wakeup = chdev_write_wakeup,
};

static int chdev_open(struct inode *inode, struct file *file)
{
	struct chdev_priv *priv =
		container_of(inode->i_cdev, struct chdev_priv, cdev);

	file->private_data = priv;
	return nonseekable_open(inode, file);
}

static ssize_t chdev_read(struct file *file, char __user *ubuf, size_t cnt,
			  loff_t *ppos)
{
	struct chdev_priv *priv = file->private_data;
	unsigned int copied = 0;
	int ret;

	if (cnt == 0)
		return 0;

	ret = wait_event_interruptible(priv->rwait,
				       !kfifo_is_empty(&priv->rx_fifo));
	if (ret)
		return ret;

	ret = kfifo_to_user(&priv->rx_fifo, ubuf, cnt, &copied);
	if (ret)
		return ret;

	return copied ? copied : -EAGAIN;
}

static ssize_t chdev_write(struct file *file, const char __user *ubuf,
			   size_t cnt, loff_t *ppos)
{
	struct chdev_priv *priv = file->private_data;
	u8 *buf;
	size_t len = min_t(size_t, cnt, CHDEV_MAX_WRITE);
	int ret;

	if (cnt == 0)
		return 0;

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, ubuf, len)) {
		kfree(buf);
		return -EFAULT;
	}

	mutex_lock(&priv->write_lock);
	ret = serdev_device_write(priv->serdev, buf, len,
				  msecs_to_jiffies(3000));
	mutex_unlock(&priv->write_lock);

	kfree(buf);

	/* serdev_device_write returns 0 on success (all data queued),
	 * negative on error, never a positive byte count */
	if (ret < 0)
		return ret;

	atomic_long_add(len, &priv->tx_bytes);
	return len;
}

static unsigned int chdev_poll(struct file *file, poll_table *wait)
{
	struct chdev_priv *priv = file->private_data;
	unsigned int mask = POLLOUT | POLLWRNORM;

	poll_wait(file, &priv->rwait, wait);
	if (!kfifo_is_empty(&priv->rx_fifo))
		mask |= POLLIN | POLLRDNORM;
	return mask;
}

static long chdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct chdev_priv *priv = file->private_data;
	void __user *uarg = (void __user *)arg;
	struct chdev_stats st;
	struct chdev_status status;
	u32 u;

	switch (cmd) {
	case CHDEV_IOC_GET_STATS:
		memset(&st, 0, sizeof(st));
		st.rx_bytes = atomic_long_read(&priv->rx_bytes);
		st.tx_bytes = atomic_long_read(&priv->tx_bytes);
		st.overruns = atomic_long_read(&priv->overruns);
		st.ch = priv->ch;
		st.baud = priv->baud;
		if (copy_to_user(uarg, &st, sizeof(st)))
			return -EFAULT;
		return 0;

	case CHDEV_IOC_GET_STATUS:
		memset(&status, 0, sizeof(status));
		status.ch = priv->ch;
		status.baud = priv->baud;
		status.opened = priv->opened;
		if (copy_to_user(uarg, &status, sizeof(status)))
			return -EFAULT;
		return 0;

	case CHDEV_IOC_SET_BAUD:
		if (get_user(u, (u32 __user *)uarg))
			return -EFAULT;
		if (u < 2400 || u > 12000000)
			return -EINVAL;
		u = serdev_device_set_baudrate(priv->serdev, u);
		priv->baud = u;
		return 0;

	case CHDEV_IOC_FLUSH:
		kfifo_reset(&priv->rx_fifo);
		return 0;
	}

	return -ENOTTY;
}

static const struct file_operations chdev_fops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.open = chdev_open,
	.read = chdev_read,
	.write = chdev_write,
	.poll = chdev_poll,
	.unlocked_ioctl = chdev_ioctl,
};

static int chdev_probe(struct serdev_device *serdev)
{
	struct device_node *np = serdev->dev.of_node, *parent;
	struct chdev_priv *priv;
	char base[16];
	bool dup = false;
	int i, ret;

	priv = devm_kzalloc(&serdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->serdev = serdev;
	priv->baud = 9600;
	of_property_read_u32(np, "current-speed", &priv->baud);

	parent = of_get_parent(np);
	if (parent) {
		u32 ch = 0;

		of_property_read_u32(parent, "reg", &ch);
		priv->ch = ch;
		of_node_put(parent);
	}

	strlcpy(base, np->name, sizeof(base));

	if (kfifo_alloc(&priv->rx_fifo, CHDEV_RX_FIFO_SIZE, GFP_KERNEL))
		return -ENOMEM;
	init_waitqueue_head(&priv->rwait);
	mutex_init(&priv->write_lock);

	/* device node name: dt node name, _<ch> suffix on duplicates */
	mutex_lock(&chdev_names_lock);
	for (i = 0; i < CHDEV_MAX_DEVS; i++)
		if (chdev_names[i][0] && !strcmp(chdev_names[i], base))
			dup = true;
	for (i = 0; i < CHDEV_MAX_DEVS; i++)
		if (!chdev_names[i][0])
			break;
	mutex_unlock(&chdev_names_lock);
	if (i >= CHDEV_MAX_DEVS) {
		ret = -ENOSPC;
		goto err_fifo;
	}
	if (dup)
		snprintf(chdev_names[i], sizeof(chdev_names[i]), "%s_%d",
			 base, priv->ch);
	else
		strlcpy(chdev_names[i], base, sizeof(chdev_names[i]));
	strlcpy(priv->name, chdev_names[i], sizeof(priv->name));
	priv->name_slot = i;

	priv->devt = MKDEV(MAJOR(chdev_devt), priv->ch);
	cdev_init(&priv->cdev, &chdev_fops);
	priv->cdev.owner = THIS_MODULE;
	ret = cdev_add(&priv->cdev, priv->devt, 1);
	if (ret)
		goto err_name;

	priv->chrdev = device_create(chdev_class, &serdev->dev, priv->devt,
				     priv, "%s", priv->name);
	if (IS_ERR(priv->chrdev)) {
		ret = PTR_ERR(priv->chrdev);
		goto err_cdev;
	}

	serdev_device_set_drvdata(serdev, priv);
	serdev_device_set_client_ops(serdev, &chdev_serdev_ops);

	ret = serdev_device_open(serdev);
	if (ret)
		goto err_dev;

	serdev_device_set_baudrate(serdev, priv->baud);
	priv->opened = true;

	dev_info(&serdev->dev, "ch%d %s: /dev/%s (%u bps), major %d minor %d\n",
		 priv->ch, base, priv->name, priv->baud,
		 MAJOR(priv->devt), MINOR(priv->devt));
	return 0;

err_dev:
	device_destroy(chdev_class, priv->devt);
err_cdev:
	cdev_del(&priv->cdev);
err_name:
	mutex_lock(&chdev_names_lock);
	chdev_names[priv->name_slot][0] = '\0';
	mutex_unlock(&chdev_names_lock);
err_fifo:
	kfifo_free(&priv->rx_fifo);
	return ret;
}

static void chdev_remove(struct serdev_device *serdev)
{
	struct chdev_priv *priv = serdev_device_get_drvdata(serdev);

	priv->opened = false;
	serdev_device_close(serdev);
	device_destroy(chdev_class, priv->devt);
	cdev_del(&priv->cdev);

	mutex_lock(&chdev_names_lock);
	chdev_names[priv->name_slot][0] = '\0';
	mutex_unlock(&chdev_names_lock);

	kfifo_free(&priv->rx_fifo);
}

static const struct of_device_id chdev_of_match[] = {
	{ .compatible = "custom,atgm336h" },
	{ .compatible = "custom,rs485" },
	{ .compatible = "custom,rv1106-link" },
	{ .compatible = "custom,n32s032-u0" },
	{ .compatible = "custom,n32s032-u3" },
	{ .compatible = "custom,m528h-ws63" },
	{ }
};
MODULE_DEVICE_TABLE(of, chdev_of_match);

static struct serdev_device_driver chdev_driver = {
	.probe = chdev_probe,
	.remove = chdev_remove,
	.driver = {
		.name = "ch9437-dev",
		.of_match_table = of_match_ptr(chdev_of_match),
	},
};

static int __init chdev_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&chdev_devt, 0, CHDEV_MAX_DEVS, "chdev");
	if (ret)
		return ret;

	chdev_class = class_create(THIS_MODULE, "chdev");
	if (IS_ERR(chdev_class)) {
		ret = PTR_ERR(chdev_class);
		goto err_region;
	}

	ret = serdev_device_driver_register(&chdev_driver);
	if (ret)
		goto err_class;

	return 0;

err_class:
	class_destroy(chdev_class);
err_region:
	unregister_chrdev_region(chdev_devt, CHDEV_MAX_DEVS);
	return ret;
}

static void __exit chdev_exit(void)
{
	serdev_device_driver_unregister(&chdev_driver);
	class_destroy(chdev_class);
	unregister_chrdev_region(chdev_devt, CHDEV_MAX_DEVS);
}

module_init(chdev_init);
module_exit(chdev_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CH9437 sub-uart generic transport device driver");
MODULE_AUTHOR("chh");
