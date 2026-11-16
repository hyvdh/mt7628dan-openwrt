#ifndef _CH9437_H
#define _CH9437_H

#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/serdev.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/tty_flip.h>

#define CH9437_DRV_NAME		"ch9437"
#define CH9437_NPORTS		7

/* CH9437 serial base clock (internal or 16MHz x12, datasheet ch.6) */
#define CH9437_BASECLK		192000000
#define CH9437_SUB_RX_FIFO	2048
#define CH9437_SUB_TX_FIFO	1536

/* host link frame sync bytes (datasheet 8.7) */
#define CH9437_SYNC0		0x57
#define CH9437_SYNC1		0xAB

/* global registers (datasheet 7.2 - 7.5) */
#define CH9437_R_TNOW_CTRL1	0x41	/* TNOW0-3: [7:4] invert, [3:0] enable */
#define CH9437_R_FIFO_CTRL	0x42	/* bit4: 0=RX/1=TX fifo, [3:0] channel */
#define CH9437_R_FIFO_CNT_L	0x43
#define CH9437_R_FIFO_CNT_H	0x44
#define CH9437_R_IO_SEL		0x45	/* 4-byte atomic pin mux (datasheet 8.10) */
#define CH9437_R_TNOW_CTRL2	0x47	/* TNOW4-7: [7:4] invert, [3:0] enable */
#define CH9437_R_CLK_CTRL	0x48	/* bit0: 1=external crystal */
#define CH9437_R_GPIO_EN0	0x50	/* GPIO7-0 enable */
#define CH9437_R_GPIO_DIRMOD2	0x56	/* GPIO5-4 mode: [7:4] GPIO5 */
#define CH9437_R_GPIO_SET0	0x5c	/* write 1 -> output high */
#define CH9437_R_GPIO_RESET0	0x5e	/* write 1 -> output low */
#define CH9437_R_VER		0x65	/* 4 bytes, VER[3]=0x8D */

/* sub-uart register offsets, base = channel * 8 (datasheet 7.1) */
#define CH9437_URBR		0x00
#define CH9437_UTHR		0x00
#define CH9437_UIER		0x01
#define CH9437_UFCR		0x02
#define CH9437_ULCR		0x03
#define CH9437_UMCR		0x04
#define CH9437_ULSR		0x05
#define CH9437_UDLL		0x00	/* DLAB=1 */
#define CH9437_UDLM		0x01	/* DLAB=1 */

/* IO_SEL commands (datasheet 8.10) */
#define CH9437_IOSEL_SET_MUX	0x01
#define CH9437_IOSEL_GET_MUX	0x81
#define CH9437_IOSEL_SET_DEF	0x03
#define CH9437_IOSEL_GET_DEF	0x83

/* default pin function addresses: UART1..7 = 2..8, HSE XI/XO = 9 */
#define CH9437_DEF_PIN(ch)	(1 + (ch))
#define CH9437_DEF_PIN_HSE	9
/* mux pin function addresses: TNOW0-7 = 2..9, GPIO0-15 = 26..41 */
#define CH9437_MUX_TNOW(n)	(2 + (n))
#define CH9437_MUX_GPIO(n)	(26 + (n))

/* sub-uart LSR bits (datasheet 7.1) */
#define CH9437_LSR_TEMT		0x40

enum ch9437_rs485_mode {
	CH9437_RS485_NONE = 0,
	CH9437_RS485_TNOW,		/* hardware auto via TNOWn pin */
	CH9437_RS485_GPIO,		/* software via CH9437 GPIO */
};

struct ch9437_chip;

struct ch9437_port {
	struct ch9437_chip *chip;
	struct device_node *np;
	int ch;
	bool opened;
	bool serdev_mode;
	bool registered;
	unsigned int baud;

	struct kfifo tx_fifo;
	spinlock_t tx_lock;

	/* serdev consumer */
	struct serdev_controller *ctrl;
	/* tty consumer (debug fallback) */
	struct uart_port uport;

	int rs485_mode;
	int rs485_gpio;
	bool rs485_enabled;
	bool rs485_gpio_active;
};

struct ch9437_chip {
	struct device *dev;
	void __iomem *membase;
	unsigned int uartclk;		/* host uart input clock */
	unsigned int host_baud;		/* host link baud rate */
	bool ext_clock;
	unsigned int poll_ms;

	/* command engine: fully polled, strictly serialized */
	struct mutex cmd_lock;

	u8 *rbr_buf;			/* worker rbr staging */
	u8 *thr_frame;			/* worker thr frame staging */

	u8 ver[4];
	unsigned long stray_bytes;
	unsigned long cmd_timeouts;

	struct ch9437_port ports[CH9437_NPORTS + 1];
	struct delayed_work poll_work;
};

/* frame helper: plain register access header */
static inline void ch9437_mk_hdr(u8 *f, u8 reg)
{
	f[0] = CH9437_SYNC0;
	f[1] = CH9437_SYNC1;
	f[2] = reg;
}

/* sub-uart register address of channel ch */
static inline u8 ch9437_ureg(int ch, int off)
{
	return (ch << 3) + off;
}

/* ch9437_core.c */
int ch9437_cmd_exec(struct ch9437_chip *chip, const u8 *frame, int flen,
		    u8 *rsp, int rlen);
int ch9437_reg_write(struct ch9437_chip *chip, u8 reg, const u8 *data, int len);
int ch9437_reg_read(struct ch9437_chip *chip, u8 reg, u8 *buf, int len);
int ch9437_set_baud(struct ch9437_chip *chip, int ch, unsigned int baud);
int ch9437_port_drained(struct ch9437_port *port, int timeout_ms);
void ch9437_kick(struct ch9437_chip *chip);
void ch9437_rs485_apply(struct ch9437_port *port, bool enable);

/* ch9437_tty.c */
int ch9437_tty_init(void);
void ch9437_tty_exit(void);
int ch9437_tty_add_port(struct ch9437_port *port);
void ch9437_tty_remove_port(struct ch9437_port *port);

/* ch9437_serdev.c */
int ch9437_serdev_add_port(struct ch9437_port *port);
void ch9437_serdev_remove_port(struct ch9437_port *port);

/* ioctl interface (ttyCHn nodes, kernel + userspace shared) */
#define CH9437_IOC_MAGIC	'W'

struct ch9437_ioc_reg {
	__u8 reg;
	__u8 len;
	__u8 data[8];
};

struct ch9437_ioc_stats {
	__u64 stray_bytes;
	__u64 cmd_timeouts;
	__u32 poll_ms;
	__u32 host_baud;
};

#define CH9437_IOC_GET_VERSION	_IOR(CH9437_IOC_MAGIC, 1, __u32)
#define CH9437_IOC_REG_READ	_IOWR(CH9437_IOC_MAGIC, 2, struct ch9437_ioc_reg)
#define CH9437_IOC_REG_WRITE	_IOW(CH9437_IOC_MAGIC, 3, struct ch9437_ioc_reg)
#define CH9437_IOC_SET_POLL	_IOW(CH9437_IOC_MAGIC, 4, __u32)
#define CH9437_IOC_GET_STATS	_IOR(CH9437_IOC_MAGIC, 5, struct ch9437_ioc_stats)

#endif /* _CH9437_H */
