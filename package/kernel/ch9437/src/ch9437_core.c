#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/serial_reg.h>

#include "ch9437.h"

static unsigned int poll_ms = 10;
module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "channel poll interval in ms");

/* ------------------------------------------------------------------ */
/* host uart (MT7628 UART1, 16550, reg-shift=2, reg-io-width=4)        */
/* ------------------------------------------------------------------ */

static inline u32 ch9437_hw_rd(struct ch9437_chip *chip, int reg)
{
	return readl(chip->membase + (reg << 2));
}

static inline void ch9437_hw_wr(struct ch9437_chip *chip, int reg, u32 v)
{
	writel(v, chip->membase + (reg << 2));
}

/* polled transfer, probe time only (irq not yet requested) */

static int ch9437_raw_send(struct ch9437_chip *chip, const u8 *buf, int len)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(200);
	int i;

	for (i = 0; i < len; i++) {
		while (!(ch9437_hw_rd(chip, UART_LSR) & UART_LSR_THRE)) {
			if (time_after(jiffies, deadline))
				return -ETIMEDOUT;
			udelay(5);
		}
		ch9437_hw_wr(chip, UART_TX, buf[i]);
	}
	return 0;
}

static int ch9437_raw_recv(struct ch9437_chip *chip, u8 *buf, int len,
			   int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	int i = 0;

	while (i < len) {
		if (ch9437_hw_rd(chip, UART_LSR) & UART_LSR_DR) {
			buf[i++] = ch9437_hw_rd(chip, UART_RX);
		} else if (time_after(jiffies, deadline)) {
			return -ETIMEDOUT;
		} else {
			usleep_range(100, 200);
		}
	}
	return 0;
}

/* partial receive: *got always reports how many bytes arrived, return
 * value only tells whether the full length was reached */
static int ch9437_raw_recv_part(struct ch9437_chip *chip, u8 *buf, int len,
				int timeout_ms, int *got)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	int i = 0;

	while (i < len) {
		if (ch9437_hw_rd(chip, UART_LSR) & UART_LSR_DR) {
			buf[i++] = ch9437_hw_rd(chip, UART_RX);
		} else if (time_after(jiffies, deadline)) {
			break;
		} else {
			udelay(50);
		}
	}
	*got = i;
	return (i == len) ? 0 : -ETIMEDOUT;
}

/* raw register access (probe time only) */

static int ch9437_raw_reg_write(struct ch9437_chip *chip, u8 reg,
				const u8 *data, int len)
{
	u8 frame[3 + 8];

	if (len < 0 || len > 8)
		return -EINVAL;
	ch9437_mk_hdr(frame, reg | 0x80);
	memcpy(&frame[3], data, len);
	return ch9437_raw_send(chip, frame, len + 3);
}

static int ch9437_raw_reg_read(struct ch9437_chip *chip, u8 reg, u8 *buf,
			       int len)
{
	u8 frame[3];

	ch9437_mk_hdr(frame, reg);
	if (ch9437_raw_send(chip, frame, 3))
		return -ETIMEDOUT;
	return ch9437_raw_recv(chip, buf, len, 100);
}

/* ------------------------------------------------------------------ */
/* chip bring-up (probe, polled)                                       */
/* ------------------------------------------------------------------ */

static int ch9437_check_version_raw(struct ch9437_chip *chip)
{
	int i;

	for (i = 0; i < 5; i++) {
		if (!ch9437_raw_reg_read(chip, CH9437_R_VER, chip->ver, 4) &&
		    chip->ver[3] == 0x8D &&
		    chip->ver[2] == (u8)(chip->ver[0] + chip->ver[1]))
			return 0;
		msleep(2);
	}
	return -ENODEV;
}

static int ch9437_io_sel_raw(struct ch9437_chip *chip, u8 cmd, u8 addr, u8 val)
{
	u8 wr[4] = { cmd, addr, val, 0xA5 };
	u8 rd[4];
	int i, got;

	ch9437_raw_reg_write(chip, CH9437_R_IO_SEL, wr, 4);

	/* strategy 1: poll the register with a plain read; the response
	 * carries the 0x5A flag once the operation completed */
	for (i = 0; i < 5; i++) {
		u8 f[3];

		msleep(1);
		ch9437_mk_hdr(f, CH9437_R_IO_SEL);
		ch9437_raw_send(chip, f, 3);
		memset(rd, 0, sizeof(rd));
		got = 0;
		ch9437_raw_recv_part(chip, rd, 4, 10, &got);
		if (got >= 3 && (rd[2] == 0x5A || rd[3] == 0x5A))
			return 0;
	}

	/* strategy 2: poll with the GET command; the chip replies with
	 * [addr][state][0x5A] as the ack */
	{
		u8 get = (cmd == CH9437_IOSEL_SET_MUX) ?
				CH9437_IOSEL_GET_MUX : CH9437_IOSEL_GET_DEF;
		u8 gw[4] = { get, addr, 0x00, 0xA5 };

		for (i = 0; i < 5; i++) {
			msleep(1);
			ch9437_raw_reg_write(chip, CH9437_R_IO_SEL, gw, 4);
			memset(rd, 0, sizeof(rd));
			got = 0;
			ch9437_raw_recv_part(chip, rd, 4, 20, &got);
			if (got >= 3 && (rd[2] == 0x5A || rd[3] == 0x5A))
				return 0;
		}
		dev_err(chip->dev,
			"io_sel cmd=%02x addr=%02x: no ack, last rd=%02x %02x %02x %02x (got %d)\n",
			cmd, addr, rd[0], rd[1], rd[2], rd[3], got);
	}
	return -EIO;
}

static int ch9437_raw_set_baud(struct ch9437_chip *chip, int ch,
			       unsigned int baud)
{
	u32 t = DIV_ROUND_CLOSEST(CH9437_BASECLK, baud);
	u8 v;

	if (!t || t > 0xffff)
		return -EINVAL;

	v = 0x83;	/* DLAB=1, 8N1 */
	ch9437_raw_reg_write(chip, ch9437_ureg(ch, CH9437_ULCR), &v, 1);
	v = t & 0xff;
	ch9437_raw_reg_write(chip, ch9437_ureg(ch, CH9437_UDLL), &v, 1);
	v = (t >> 8) & 0xff;
	ch9437_raw_reg_write(chip, ch9437_ureg(ch, CH9437_UDLM), &v, 1);
	v = 0x03;
	ch9437_raw_reg_write(chip, ch9437_ureg(ch, CH9437_ULCR), &v, 1);
	return 0;
}

static int ch9437_bringup(struct ch9437_chip *chip)
{
	struct ch9437_port *port;
	u32 div, t;
	u8 f[8], v;
	int ch, ret;

	/* host uart: requested baud, 8N1, fifo on */
	ch9437_hw_wr(chip, UART_IER, 0);
	ch9437_hw_wr(chip, UART_LCR, UART_LCR_DLAB);
	div = chip->uartclk / (16 * chip->host_baud);
	if (!div || div > 0xffff)
		return -EINVAL;
	ch9437_hw_wr(chip, UART_DLL, div & 0xff);
	ch9437_hw_wr(chip, UART_DLM, (div >> 8) & 0xff);
	ch9437_hw_wr(chip, UART_LCR, 0x03);
	ch9437_hw_wr(chip, UART_FCR, UART_FCR_ENABLE_FIFO |
		     UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT |
		     UART_FCR_TRIGGER_14);
	while (ch9437_hw_rd(chip, UART_LSR) & UART_LSR_DR)
		(void)ch9437_hw_rd(chip, UART_RX);

	/* auto baud lock: host sends 0x55 (datasheet 8.7) */
	for (ch = 0; ch < 3; ch++) {
		u8 c = 0x55;
		ch9437_raw_send(chip, &c, 1);
		msleep(2);
	}

	/* link check on internal clock */
	ret = ch9437_check_version_raw(chip);
	if (ret) {
		dev_err(chip->dev, "no response from CH9437 at %u bps\n",
			chip->host_baud);
		return ret;
	}

	/* switch to external crystal, then relock host divisor exactly */
	if (chip->ext_clock) {
		/* XI/XO pins must be enabled before the crystal can
		 * oscillate (datasheet 8.10, default pin addr 9) */
		ret = ch9437_io_sel_raw(chip, CH9437_IOSEL_SET_DEF,
					CH9437_DEF_PIN_HSE, 1);
		if (ret)
			return ret;
		msleep(10);

		v = 0x01;
		ch9437_raw_reg_write(chip, CH9437_R_CLK_CTRL, &v, 1);
		msleep(10);
		t = DIV_ROUND_CLOSEST(CH9437_BASECLK, chip->host_baud);
		f[0] = CH9437_SYNC0;
		f[1] = CH9437_SYNC1;
		f[2] = 0x80;		/* write host uart0 DLL, 2 data bytes */
		f[3] = t & 0xff;
		f[4] = (t >> 8) & 0xff;
		ch9437_raw_send(chip, f, 5);
		msleep(2);
		ret = ch9437_check_version_raw(chip);
		if (ret) {
			dev_err(chip->dev, "link lost after clock switch\n");
			return ret;
		}
	}

	/* pin function enables (datasheet 8.10) */
	for (ch = 1; ch <= CH9437_NPORTS; ch++) {
		port = &chip->ports[ch];
		if (!port->np)
			continue;
		ret = ch9437_io_sel_raw(chip, CH9437_IOSEL_SET_DEF,
					CH9437_DEF_PIN(ch), 1);
		if (ret)
			return ret;
		if (port->rs485_mode == CH9437_RS485_TNOW) {
			ret = ch9437_io_sel_raw(chip, CH9437_IOSEL_SET_MUX,
						CH9437_MUX_TNOW(ch), 1);
			if (ret)
				return ret;
		}
		if (port->rs485_mode == CH9437_RS485_GPIO) {
			ret = ch9437_io_sel_raw(chip, CH9437_IOSEL_SET_MUX,
						CH9437_MUX_GPIO(port->rs485_gpio), 1);
			if (ret)
				return ret;
		}
	}

	/* per channel defaults + rs485 configuration */
	for (ch = 1; ch <= CH9437_NPORTS; ch++) {
		port = &chip->ports[ch];
		if (!port->np)
			continue;

		v = 0x80;	/* IER.RESET: soft reset sub uart */
		ch9437_raw_reg_write(chip, ch9437_ureg(ch, CH9437_UIER), &v, 1);
		v = 0x07;	/* FCR: fifo enable + reset both */
		ch9437_raw_reg_write(chip, ch9437_ureg(ch, CH9437_UFCR), &v, 1);
		ch9437_raw_set_baud(chip, ch, port->baud);

		if (port->rs485_mode == CH9437_RS485_TNOW) {
			u8 reg = (ch <= 3) ? CH9437_R_TNOW_CTRL1
					   : CH9437_R_TNOW_CTRL2;
			int bit = ch & 3;
			if (!ch9437_raw_reg_read(chip, reg, &v, 1))
				v |= (1 << bit);
			else
				v = (1 << bit);
			ch9437_raw_reg_write(chip, reg, &v, 1);
			port->rs485_enabled = true;
		} else if (port->rs485_mode == CH9437_RS485_GPIO) {
			int g = port->rs485_gpio;
			u8 reg = 0x54 + (g / 2);
			u8 nib = (g & 1) ? 0x30 : 0x03; /* push-pull output */

			if (!ch9437_raw_reg_read(chip, CH9437_R_GPIO_EN0, &v, 1))
				v |= (1 << (g & 7));
			else
				v = (1 << (g & 7));
			ch9437_raw_reg_write(chip, CH9437_R_GPIO_EN0, &v, 1);
			if (!ch9437_raw_reg_read(chip, reg, &v, 1))
				v |= nib;
			else
				v = nib;
			ch9437_raw_reg_write(chip, reg, &v, 1);
			v = (1 << (g & 7));	/* gpio low = receive */
			ch9437_raw_reg_write(chip, CH9437_R_GPIO_RESET0, &v, 1);
			port->rs485_enabled = true;
		}
	}

	dev_info(chip->dev, "CH9437 link up: ver %x.%02x, host %u bps%s\n",
		 chip->ver[1], chip->ver[0], chip->host_baud,
		 chip->ext_clock ? ", ext 16MHz clock" : "");
	return 0;
}

/* ------------------------------------------------------------------ */
/* runtime command engine (fully polled)                               */
/*                                                                    */
/* This uart (MT7628 UART-Lite2 at 0xd00) asserts its interrupt line  */
/* whenever IER != 0 and the transmit holding register is empty,      */
/* regardless of the THRI enable bit.  Keeping IER at 0 permanently   */
/* is therefore the only storm free configuration.  The host link is */
/* a strict command/response protocol, so polled exchanges cost the  */
/* same cpu time an interrupt driven engine would: responses are     */
/* bounded and expected, and the 16 byte hw rx fifo is drained       */
/* synchronously during reception.                                    */
/* ------------------------------------------------------------------ */

static int ch9437_cmd_xmit(struct ch9437_chip *chip, const u8 *frame,
			   int flen, unsigned long deadline)
{
	int i = 0;

	while (i < flen) {
		if (ch9437_hw_rd(chip, UART_LSR) & UART_LSR_THRE) {
			int n = min(16, flen - i);

			while (n--)
				ch9437_hw_wr(chip, UART_TX, frame[i++]);
		} else {
			if (time_after(jiffies, deadline))
				return -ETIMEDOUT;
			cpu_relax();
		}
	}
	return 0;
}

static int ch9437_cmd_recv(struct ch9437_chip *chip, u8 *rsp, int rlen,
			   unsigned long deadline)
{
	int i = 0;

	while (i < rlen) {
		if (ch9437_hw_rd(chip, UART_LSR) & UART_LSR_DR) {
			rsp[i++] = ch9437_hw_rd(chip, UART_RX);
		} else {
			if (time_after(jiffies, deadline))
				return -ETIMEDOUT;
			cpu_relax();
		}
	}
	return rlen;
}

int ch9437_cmd_exec(struct ch9437_chip *chip, const u8 *frame, int flen,
		    u8 *rsp, int rlen)
{
	int ret = -ETIMEDOUT, try;

	mutex_lock(&chip->cmd_lock);

	for (try = 0; try < 3; try++) {
		unsigned long deadline = jiffies + msecs_to_jiffies(200);

		/* drop stale rx from a previous failed exchange */
		while (ch9437_hw_rd(chip, UART_LSR) & UART_LSR_DR) {
			(void)ch9437_hw_rd(chip, UART_RX);
			chip->stray_bytes++;
		}

		ret = ch9437_cmd_xmit(chip, frame, flen, deadline);
		if (ret)
			continue;

		if (!rlen) {
			ret = 0;
			break;
		}

		ret = ch9437_cmd_recv(chip, rsp, rlen, deadline);
		if (ret == rlen)
			break;

		chip->cmd_timeouts++;
	}

	mutex_unlock(&chip->cmd_lock);
	return ret;
}

int ch9437_reg_write(struct ch9437_chip *chip, u8 reg, const u8 *data, int len)
{
	u8 frame[3 + 8];

	if (len < 0 || len > 8)
		return -EINVAL;
	ch9437_mk_hdr(frame, reg | 0x80);
	memcpy(&frame[3], data, len);
	return ch9437_cmd_exec(chip, frame, len + 3, NULL, 0);
}

int ch9437_reg_read(struct ch9437_chip *chip, u8 reg, u8 *buf, int len)
{
	u8 frame[3];
	int ret;

	ch9437_mk_hdr(frame, reg);
	ret = ch9437_cmd_exec(chip, frame, 3, buf, len);
	return (ret == len) ? 0 : ret;
}

/* T = round(baseclk / baud) is the 16-bit DLL:DLM value with the
 * 4-bit fractional divisor in its low bits (datasheet 7.1) */
int ch9437_set_baud(struct ch9437_chip *chip, int ch, unsigned int baud)
{
	u32 t = DIV_ROUND_CLOSEST(CH9437_BASECLK, baud);
	u8 v;
	int ret;

	if (!t || t > 0xffff)
		return -EINVAL;

	v = 0x83;
	ret = ch9437_reg_write(chip, ch9437_ureg(ch, CH9437_ULCR), &v, 1);
	if (ret)
		return ret;
	v = t & 0xff;
	ret = ch9437_reg_write(chip, ch9437_ureg(ch, CH9437_UDLL), &v, 1);
	if (ret)
		return ret;
	v = (t >> 8) & 0xff;
	ret = ch9437_reg_write(chip, ch9437_ureg(ch, CH9437_UDLM), &v, 1);
	if (ret)
		return ret;
	v = 0x03;
	return ch9437_reg_write(chip, ch9437_ureg(ch, CH9437_ULCR), &v, 1);
}

static int ch9437_gpio_out(struct ch9437_chip *chip, int gpio, int val)
{
	u8 b = 1 << (gpio & 7);

	return ch9437_reg_write(chip, val ? CH9437_R_GPIO_SET0
					  : CH9437_R_GPIO_RESET0, &b, 1);
}

void ch9437_rs485_apply(struct ch9437_port *port, bool enable)
{
	struct ch9437_chip *chip = port->chip;
	u8 reg, v;
	int bit;

	if (port->rs485_mode == CH9437_RS485_TNOW) {
		reg = (port->ch <= 3) ? CH9437_R_TNOW_CTRL1
				      : CH9437_R_TNOW_CTRL2;
		bit = port->ch & 3;
		if (!ch9437_reg_read(chip, reg, &v, 1))
			v = enable ? (v | (1 << bit)) : (v & ~(1 << bit));
		else
			v = enable ? (1 << bit) : 0;
		ch9437_reg_write(chip, reg, &v, 1);
	} else if (port->rs485_mode == CH9437_RS485_GPIO) {
		if (!enable && port->rs485_gpio_active) {
			ch9437_gpio_out(chip, port->rs485_gpio, 0);
			port->rs485_gpio_active = false;
		}
	}
	port->rs485_enabled = enable;
}

int ch9437_port_drained(struct ch9437_port *port, int timeout_ms)
{
	struct ch9437_chip *chip = port->chip;
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

	while (!time_after(jiffies, deadline)) {
		if (!kfifo_is_empty(&port->tx_fifo))
			goto again;
		else {
			u8 lsr;
			if (!ch9437_reg_read(chip,
					ch9437_ureg(port->ch, CH9437_ULSR),
					&lsr, 1) && (lsr & CH9437_LSR_TEMT))
				return 0;
		}
again:
		msleep(5);
	}
	return -ETIMEDOUT;
}

void ch9437_kick(struct ch9437_chip *chip)
{
	mod_delayed_work(system_wq, &chip->poll_work, 0);
}

/* ------------------------------------------------------------------ */
/* poll worker                                                         */
/* ------------------------------------------------------------------ */

static void ch9437_dispatch_rx(struct ch9437_port *port, const u8 *buf, int len)
{
	if (port->serdev_mode) {
		if (port->ctrl)
			serdev_controller_receive_buf(port->ctrl, buf, len);
	} else if (port->uport.state) {
		tty_insert_flip_string(&port->uport.state->port, buf, len);
		tty_flip_buffer_push(&port->uport.state->port);
	}
}

static void ch9437_poll(struct work_struct *work)
{
	struct ch9437_chip *chip =
		container_of(to_delayed_work(work), struct ch9437_chip,
			     poll_work);
	int ch;

	for (ch = 1; ch <= CH9437_NPORTS; ch++) {
		struct ch9437_port *port = &chip->ports[ch];
		u8 v, lo, hi, lsr;
		int cnt, n;

		if (!port->opened)
			continue;

		/* rx: query fifo level then bulk read rbr */
		v = ch;
		if (ch9437_reg_write(chip, CH9437_R_FIFO_CTRL, &v, 1))
			continue;
		if (ch9437_reg_read(chip, CH9437_R_FIFO_CNT_L, &lo, 1))
			continue;
		if (ch9437_reg_read(chip, CH9437_R_FIFO_CNT_H, &hi, 1))
			continue;
		cnt = lo | (hi << 8);
		if (cnt > 0) {
			u8 f[5];
			int len = min_t(int, cnt, CH9437_SUB_RX_FIFO);

			ch9437_mk_hdr(f, ch9437_ureg(ch, CH9437_URBR));
			f[3] = (len >> 8) & 0xff;
			f[4] = len & 0xff;
			if (ch9437_cmd_exec(chip, f, 5, chip->rbr_buf, len)
			    == len)
				ch9437_dispatch_rx(port, chip->rbr_buf, len);
		}

		/* tx: push pending data into sub uart fifo */
		if (!kfifo_is_empty(&port->tx_fifo)) {
			if (port->rs485_mode == CH9437_RS485_GPIO &&
			    port->rs485_enabled && !port->rs485_gpio_active) {
				ch9437_gpio_out(chip, port->rs485_gpio, 1);
				port->rs485_gpio_active = true;
			}
			v = 0x10 | ch;
			if (!ch9437_reg_write(chip, CH9437_R_FIFO_CTRL, &v, 1)
			    && !ch9437_reg_read(chip, CH9437_R_FIFO_CNT_L,
						&lo, 1)
			    && !ch9437_reg_read(chip, CH9437_R_FIFO_CNT_H,
						&hi, 1)) {
				cnt = lo | (hi << 8); /* free space */
				n = min_t(int, kfifo_len(&port->tx_fifo),
					  min(cnt, CH9437_SUB_TX_FIFO));
				if (n > 0) {
					unsigned long flags;

					spin_lock_irqsave(&port->tx_lock, flags);
					n = kfifo_out(&port->tx_fifo,
						      &chip->thr_frame[5], n);
					spin_unlock_irqrestore(&port->tx_lock,
							       flags);
					chip->thr_frame[0] = CH9437_SYNC0;
					chip->thr_frame[1] = CH9437_SYNC1;
					chip->thr_frame[2] =
						ch9437_ureg(ch, CH9437_UTHR)
						| 0x80;
					chip->thr_frame[3] = (n >> 8) & 0xff;
					chip->thr_frame[4] = n & 0xff;
					ch9437_cmd_exec(chip, chip->thr_frame,
							n + 5, NULL, 0);
					if (port->serdev_mode && port->ctrl) {
						if (port->ctrl->serdev)
							serdev_device_write_wakeup(
								port->ctrl->serdev);
						serdev_controller_write_wakeup(
							port->ctrl);
					}
				}
			}
		}

		/* tty: pump uart xmit ring into tx fifo */
		if (!port->serdev_mode && port->uport.state) {
			struct uart_port *uport = &port->uport;
			struct circ_buf *xmit = &uport->state->xmit;
			unsigned long flags;
			bool wakeup = false;

			spin_lock_irqsave(&uport->lock, flags);
			while (!uart_circ_empty(xmit) &&
			       kfifo_avail(&port->tx_fifo)) {
				u8 b = xmit->buf[xmit->tail];
				kfifo_in(&port->tx_fifo, &b, 1);
				xmit->tail = (xmit->tail + 1) &
					     (UART_XMIT_SIZE - 1);
				uport->icount.tx++;
			}
			if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
				wakeup = true;
			spin_unlock_irqrestore(&uport->lock, flags);
			if (wakeup)
				uart_write_wakeup(uport);
		}

		/* gpio rs485: drop direction when fully sent */
		if (port->rs485_mode == CH9437_RS485_GPIO &&
		    port->rs485_gpio_active &&
		    kfifo_is_empty(&port->tx_fifo) &&
		    !ch9437_reg_read(chip,
				ch9437_ureg(ch, CH9437_ULSR), &lsr, 1) &&
		    (lsr & CH9437_LSR_TEMT)) {
			ch9437_gpio_out(chip, port->rs485_gpio, 0);
			port->rs485_gpio_active = false;
		}
	}

	queue_delayed_work(system_wq, &chip->poll_work,
			   msecs_to_jiffies(chip->poll_ms));
}

/* ------------------------------------------------------------------ */
/* platform driver                                                     */
/* ------------------------------------------------------------------ */

static const struct of_device_id ch9437_of_match[] = {
	{ .compatible = "wch,ch9437f" },
	{ }
};
MODULE_DEVICE_TABLE(of, ch9437_of_match);

static int ch9437_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node, *child, *gc;
	struct ch9437_chip *chip;
	struct ch9437_port *port;
	struct resource *res;
	u32 val;
	int ret, ch;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	chip->poll_ms = poll_ms;
	chip->uartclk = 40000000;
	chip->host_baud = 500000;
	of_property_read_u32(np, "clock-frequency", &chip->uartclk);
	of_property_read_u32(np, "current-speed", &chip->host_baud);
	chip->ext_clock = of_property_read_bool(np, "wch,external-clock");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	chip->membase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(chip->membase))
		return PTR_ERR(chip->membase);

	mutex_init(&chip->cmd_lock);
	INIT_DELAYED_WORK(&chip->poll_work, ch9437_poll);

	chip->rbr_buf = kmalloc(CH9437_SUB_RX_FIFO, GFP_KERNEL);
	chip->thr_frame = kmalloc(CH9437_SUB_TX_FIFO + 8, GFP_KERNEL);
	if (!chip->rbr_buf || !chip->thr_frame) {
		ret = -ENOMEM;
		goto err_free;
	}

	/* parse channel nodes first: rs485 wiring is needed by bring-up */
	for_each_child_of_node(np, child) {
		u32 ch = 0;

		of_property_read_u32(child, "reg", &ch);
		if (ch < 1 || ch > CH9437_NPORTS)
			continue;

		port = &chip->ports[ch];
		port->chip = chip;
		port->ch = ch;
		port->np = of_node_get(child);
		spin_lock_init(&port->tx_lock);
		if (kfifo_alloc(&port->tx_fifo, 4096, GFP_KERNEL)) {
			ret = -ENOMEM;
			goto err_free;
		}
		port->baud = 115200;
		of_property_read_u32(child, "current-speed", &port->baud);

		if (of_property_read_bool(child, "wch,rs485-auto")) {
			port->rs485_mode = CH9437_RS485_TNOW;
		} else if (!of_property_read_u32(child, "wch,rs485-gpio",
						 &val)) {
			port->rs485_mode = CH9437_RS485_GPIO;
			port->rs485_gpio = val;
		}

		for_each_available_child_of_node(child, gc) {
			if (of_find_property(gc, "compatible", NULL)) {
				port->serdev_mode = true;
				break;
			}
		}
	}

	ret = ch9437_bringup(chip);
	if (ret)
		goto err_free;
	for (ch = 1; ch <= CH9437_NPORTS; ch++) {
		port = &chip->ports[ch];
		if (!port->np)
			continue;
		if (port->serdev_mode)
			ret = ch9437_serdev_add_port(port);
		else
			ret = ch9437_tty_add_port(port);
		if (ret)
			dev_err(chip->dev,
				"channel %d registration failed: %d\n",
				ch, ret);
		else
			port->registered = true;
	}

	platform_set_drvdata(pdev, chip);
	queue_delayed_work(system_wq, &chip->poll_work,
			   msecs_to_jiffies(chip->poll_ms));
	return 0;

err_free:
	for (ch = 1; ch <= CH9437_NPORTS; ch++) {
		port = &chip->ports[ch];
		if (port->np) {
			of_node_put(port->np);
			port->np = NULL;
		}
		kfifo_free(&port->tx_fifo);
	}
	kfree(chip->rbr_buf);
	kfree(chip->thr_frame);
	return ret;
}

static int ch9437_remove(struct platform_device *pdev)
{
	struct ch9437_chip *chip = platform_get_drvdata(pdev);
	struct ch9437_port *port;
	int ch;

	cancel_delayed_work_sync(&chip->poll_work);
	ch9437_hw_wr(chip, UART_IER, 0);

	for (ch = 1; ch <= CH9437_NPORTS; ch++) {
		port = &chip->ports[ch];
		if (!port->registered)
			continue;
		if (port->serdev_mode)
			ch9437_serdev_remove_port(port);
		else
			ch9437_tty_remove_port(port);
		port->registered = false;
	}

	for (ch = 1; ch <= CH9437_NPORTS; ch++) {
		port = &chip->ports[ch];
		if (port->np) {
			of_node_put(port->np);
			port->np = NULL;
		}
		kfifo_free(&port->tx_fifo);
	}
	kfree(chip->rbr_buf);
	kfree(chip->thr_frame);
	return 0;
}

static struct platform_driver ch9437_platform_driver = {
	.probe = ch9437_probe,
	.remove = ch9437_remove,
	.driver = {
		.name = CH9437_DRV_NAME,
		.of_match_table = of_match_ptr(ch9437_of_match),
	},
};

static int __init ch9437_module_init(void)
{
	int ret;

	ret = ch9437_tty_init();
	if (ret)
		return ret;

	ret = platform_driver_register(&ch9437_platform_driver);
	if (ret)
		ch9437_tty_exit();
	return ret;
}

static void __exit ch9437_module_exit(void)
{
	platform_driver_unregister(&ch9437_platform_driver);
	ch9437_tty_exit();
}

module_init(ch9437_module_init);
module_exit(ch9437_module_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("WCH CH9437F UART expansion chip driver");
MODULE_AUTHOR("chh");
