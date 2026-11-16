#include <linux/module.h>
#include <linux/serdev.h>

#include "ch9437.h"

static struct ch9437_port *ctrl_to_port(struct serdev_controller *ctrl)
{
	return serdev_controller_get_drvdata(ctrl);
}

static int ch9437_serdev_open(struct serdev_controller *ctrl)
{
	struct ch9437_port *port = ctrl_to_port(ctrl);

	port->opened = true;
	ch9437_kick(port->chip);
	return 0;
}

static void ch9437_serdev_close(struct serdev_controller *ctrl)
{
	struct ch9437_port *port = ctrl_to_port(ctrl);

	port->opened = false;
}

static int ch9437_serdev_write_buf(struct serdev_controller *ctrl,
				   const unsigned char *buf, size_t count)
{
	struct ch9437_port *port = ctrl_to_port(ctrl);
	unsigned long flags;
	unsigned int n;

	spin_lock_irqsave(&port->tx_lock, flags);
	n = min_t(size_t, count, kfifo_avail(&port->tx_fifo));
	n = kfifo_in(&port->tx_fifo, buf, n);
	spin_unlock_irqrestore(&port->tx_lock, flags);

	ch9437_kick(port->chip);
	return n;
}

static int ch9437_serdev_write_room(struct serdev_controller *ctrl)
{
	struct ch9437_port *port = ctrl_to_port(ctrl);

	return kfifo_avail(&port->tx_fifo);
}

static void ch9437_serdev_write_flush(struct serdev_controller *ctrl)
{
	struct ch9437_port *port = ctrl_to_port(ctrl);

	ch9437_port_drained(port, 3000);
}

static unsigned int ch9437_serdev_set_baudrate(struct serdev_controller *ctrl,
						unsigned int speed)
{
	struct ch9437_port *port = ctrl_to_port(ctrl);

	if (!ch9437_set_baud(port->chip, port->ch, speed))
		port->baud = speed;
	return port->baud;
}

static void ch9437_serdev_set_flow_control(struct serdev_controller *ctrl,
					    bool enable)
{
	struct ch9437_port *port = ctrl_to_port(ctrl);
	u8 mcr = enable ? 0x10 : 0x00;

	ch9437_reg_write(port->chip, ch9437_ureg(port->ch, CH9437_UMCR),
			 &mcr, 1);
}

static void ch9437_serdev_wait_until_sent(struct serdev_controller *ctrl,
					   long timeout)
{
	struct ch9437_port *port = ctrl_to_port(ctrl);

	ch9437_port_drained(port,
			    timeout > 0 ? jiffies_to_msecs(timeout) : 3000);
}

static int ch9437_serdev_get_tiocm(struct serdev_controller *ctrl)
{
	return TIOCM_CAR | TIOCM_DSR;
}

static int ch9437_serdev_set_tiocm(struct serdev_controller *ctrl,
				    unsigned int set, unsigned int clear)
{
	return 0;
}

static const struct serdev_controller_ops ch9437_serdev_ctrl_ops = {
	.open = ch9437_serdev_open,
	.close = ch9437_serdev_close,
	.write_buf = ch9437_serdev_write_buf,
	.write_flush = ch9437_serdev_write_flush,
	.write_room = ch9437_serdev_write_room,
	.set_baudrate = ch9437_serdev_set_baudrate,
	.set_flow_control = ch9437_serdev_set_flow_control,
	.wait_until_sent = ch9437_serdev_wait_until_sent,
	.get_tiocm = ch9437_serdev_get_tiocm,
	.set_tiocm = ch9437_serdev_set_tiocm,
};

int ch9437_serdev_add_port(struct ch9437_port *port)
{
	struct serdev_controller *ctrl;
	int ret;

	ctrl = serdev_controller_alloc(port->chip->dev, 0);
	if (!ctrl)
		return -ENOMEM;

	ctrl->ops = &ch9437_serdev_ctrl_ops;
	/* channel wrapper node: controller_add enumerates its children */
	ctrl->dev.of_node = port->np;
	serdev_controller_set_drvdata(ctrl, port);

	ret = serdev_controller_add(ctrl);
	if (ret) {
		serdev_controller_put(ctrl);
		return ret;
	}

	port->ctrl = ctrl;
	dev_info(port->chip->dev, "channel %d: serdev controller\n", port->ch);
	return 0;
}

void ch9437_serdev_remove_port(struct ch9437_port *port)
{
	if (!port->ctrl)
		return;

	port->opened = false;
	serdev_controller_remove(port->ctrl);
	serdev_controller_put(port->ctrl);
	port->ctrl = NULL;
}
