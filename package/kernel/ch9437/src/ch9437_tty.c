#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/uaccess.h>
#include <asm/ioctls.h>

#include "ch9437.h"

static struct uart_driver ch9437_uart_drv = {
	.owner = THIS_MODULE,
	.driver_name = CH9437_DRV_NAME,
	.dev_name = "ttyCH",
	.nr = CH9437_NPORTS + 1,	/* lines 1..7 used, 0 unused */
};

static struct ch9437_port *to_chport(struct uart_port *uport)
{
	return container_of(uport, struct ch9437_port, uport);
}

static unsigned int ch9437_tx_empty(struct uart_port *uport)
{
	struct ch9437_port *port = to_chport(uport);

	return kfifo_is_empty(&port->tx_fifo) ? TIOCSER_TEMT : 0;
}

static unsigned int ch9437_get_mctrl(struct uart_port *uport)
{
	return TIOCM_CAR | TIOCM_DSR;
}

static void ch9437_set_mctrl(struct uart_port *uport, unsigned int mctrl)
{
	struct ch9437_port *port = to_chport(uport);
	u8 mcr = 0;

	if (mctrl & TIOCM_RTS)
		mcr |= 0x02;
	ch9437_reg_write(port->chip, ch9437_ureg(port->ch, CH9437_UMCR),
			 &mcr, 1);
}

static void ch9437_stop_tx(struct uart_port *uport)
{
}

static void ch9437_start_tx(struct uart_port *uport)
{
	struct ch9437_port *port = to_chport(uport);

	ch9437_kick(port->chip);
}

static void ch9437_stop_rx(struct uart_port *uport)
{
}

static void ch9437_enable_ms(struct uart_port *uport)
{
}

static void ch9437_break_ctl(struct uart_port *uport, int break_state)
{
}

static int ch9437_startup(struct uart_port *uport)
{
	struct ch9437_port *port = to_chport(uport);
	int ret;

	ret = ch9437_set_baud(port->chip, port->ch, port->baud);
	if (ret)
		return ret;
	port->opened = true;
	ch9437_kick(port->chip);
	return 0;
}

static void ch9437_shutdown(struct uart_port *uport)
{
	struct ch9437_port *port = to_chport(uport);

	port->opened = false;
}

static void ch9437_set_termios(struct uart_port *uport,
			       struct ktermios *termios,
			       struct ktermios *old)
{
	struct ch9437_port *port = to_chport(uport);
	struct ch9437_chip *chip = port->chip;
	unsigned int baud, lcr = 0x03;	/* 8N1, 8-bit only chip */
	u8 v, mcr = 0;

	termios->c_cflag &= ~CSIZE;
	termios->c_cflag |= CS8;
	termios->c_cflag &= ~CMSPAR;

	if (termios->c_cflag & CSTOPB)
		lcr |= 0x04;
	if (termios->c_cflag & PARENB) {
		lcr |= 0x08;
		if (!(termios->c_cflag & PARODD))
			lcr |= 0x10;
	}

	baud = uart_get_baud_rate(uport, termios, old, 2400, 12000000);
	if (!baud)
		baud = port->baud ?: 9600;

	if (!ch9437_set_baud(chip, port->ch, baud)) {
		port->baud = baud;
		tty_termios_encode_baud_rate(termios, baud, baud);
	}

	v = lcr;
	ch9437_reg_write(chip, ch9437_ureg(port->ch, CH9437_ULCR), &v, 1);

	if (termios->c_cflag & CRTSCTS)
		mcr |= 0x10;	/* MCR.AFE auto flow control */
	ch9437_reg_write(chip, ch9437_ureg(port->ch, CH9437_UMCR), &mcr, 1);
}

static const char *ch9437_type(struct uart_port *uport)
{
	return "CH9437";
}

static void ch9437_release_port(struct uart_port *uport)
{
}

static int ch9437_request_port(struct uart_port *uport)
{
	return 0;
}

static void ch9437_config_port(struct uart_port *uport, int flags)
{
	uport->type = PORT_16550A;
}

static int ch9437_verify_port(struct uart_port *uport,
			      struct serial_struct *s)
{
	return 0;
}

static int ch9437_ioctl(struct uart_port *uport, unsigned int cmd,
			unsigned long arg)
{
	struct ch9437_port *port = to_chport(uport);
	struct ch9437_chip *chip = port->chip;
	void __user *uarg = (void __user *)arg;
	struct ch9437_ioc_reg r;
	struct ch9437_ioc_stats st;
	struct serial_rs485 rs485;
	unsigned int u;
	int ret;

	switch (cmd) {
	case TIOCSRS485:
		if (copy_from_user(&rs485, uarg, sizeof(rs485)))
			return -EFAULT;
		if (port->rs485_mode == CH9437_RS485_NONE)
			return -ENOTTY;
		ch9437_rs485_apply(port,
				   !!(rs485.flags & SER_RS485_ENABLED));
		return 0;

	case TIOCGRS485:
		memset(&rs485, 0, sizeof(rs485));
		if (port->rs485_enabled)
			rs485.flags |= SER_RS485_ENABLED;
		if (copy_to_user(uarg, &rs485, sizeof(rs485)))
			return -EFAULT;
		return 0;

	case CH9437_IOC_GET_VERSION:
		{
			u32 v = chip->ver[0] | (chip->ver[1] << 8) |
				(chip->ver[2] << 16) | (chip->ver[3] << 24);
			return put_user(v, (u32 __user *)uarg);
		}

	case CH9437_IOC_REG_READ:
		if (copy_from_user(&r, uarg, sizeof(r)))
			return -EFAULT;
		if (r.len < 1 || r.len > 8)
			return -EINVAL;
		ret = ch9437_reg_read(chip, r.reg, r.data, r.len);
		if (ret < 0)
			return ret;
		if (copy_to_user(uarg, &r, sizeof(r)))
			return -EFAULT;
		return 0;

	case CH9437_IOC_REG_WRITE:
		if (copy_from_user(&r, uarg, sizeof(r)))
			return -EFAULT;
		if (r.len < 1 || r.len > 8)
			return -EINVAL;
		return ch9437_reg_write(chip, r.reg, r.data, r.len);

	case CH9437_IOC_SET_POLL:
		if (get_user(u, (u32 __user *)uarg))
			return -EFAULT;
		if (u < 1 || u > 1000)
			return -EINVAL;
		chip->poll_ms = u;
		return 0;

	case CH9437_IOC_GET_STATS:
		memset(&st, 0, sizeof(st));
		st.stray_bytes = chip->stray_bytes;
		st.cmd_timeouts = chip->cmd_timeouts;
		st.poll_ms = chip->poll_ms;
		st.host_baud = chip->host_baud;
		if (copy_to_user(uarg, &st, sizeof(st)))
			return -EFAULT;
		return 0;
	}

	return -ENOIOCTLCMD;
}

static const struct uart_ops ch9437_uart_ops = {
	.tx_empty = ch9437_tx_empty,
	.set_mctrl = ch9437_set_mctrl,
	.get_mctrl = ch9437_get_mctrl,
	.stop_tx = ch9437_stop_tx,
	.start_tx = ch9437_start_tx,
	.stop_rx = ch9437_stop_rx,
	.enable_ms = ch9437_enable_ms,
	.break_ctl = ch9437_break_ctl,
	.startup = ch9437_startup,
	.shutdown = ch9437_shutdown,
	.set_termios = ch9437_set_termios,
	.type = ch9437_type,
	.release_port = ch9437_release_port,
	.request_port = ch9437_request_port,
	.config_port = ch9437_config_port,
	.verify_port = ch9437_verify_port,
	.ioctl = ch9437_ioctl,
};

int ch9437_tty_add_port(struct ch9437_port *port)
{
	struct uart_port *uport = &port->uport;
	int ret;

	uport->line = port->ch;
	uport->dev = port->chip->dev;
	uport->type = PORT_16550A;
	uport->fifosize = 64;
	uport->ops = &ch9437_uart_ops;
	uport->flags = UPF_BOOT_AUTOCONF;

	ret = uart_add_one_port(&ch9437_uart_drv, uport);
	if (ret)
		return ret;

	dev_info(port->chip->dev, "channel %d: ttyCH%d (debug tty)\n",
		 port->ch, port->ch);
	return 0;
}

void ch9437_tty_remove_port(struct ch9437_port *port)
{
	uart_remove_one_port(&ch9437_uart_drv, &port->uport);
}

int ch9437_tty_init(void)
{
	return uart_register_driver(&ch9437_uart_drv);
}

void ch9437_tty_exit(void)
{
	uart_unregister_driver(&ch9437_uart_drv);
}
