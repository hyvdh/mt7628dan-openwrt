#ifndef _CH9437_DEV_H
#define _CH9437_DEV_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* transport level ioctl on /dev/<chdev> nodes (userspace shared) */
#define CHDEV_IOC_MAGIC	'C'

struct chdev_stats {
	__u64 rx_bytes;
	__u64 tx_bytes;
	__u32 overruns;
	__u32 ch;
	__u32 baud;
};

struct chdev_status {
	__u32 ch;
	__u32 baud;
	__u32 opened;
	__u32 reserved;
};

#define CHDEV_IOC_GET_STATS	_IOR(CHDEV_IOC_MAGIC, 1, struct chdev_stats)
#define CHDEV_IOC_GET_STATUS	_IOR(CHDEV_IOC_MAGIC, 2, struct chdev_status)
#define CHDEV_IOC_SET_BAUD	_IOW(CHDEV_IOC_MAGIC, 3, __u32)
#define CHDEV_IOC_FLUSH		_IO(CHDEV_IOC_MAGIC, 4)

#endif /* _CH9437_DEV_H */
