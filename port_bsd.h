#ifndef _PORT_BSD_H
#define _PORT_BSD_H

void *device_private(device_t dev);

#ifndef	SD_IO_TIMEOUT
#define	SD_IO_TIMEOUT	(60 * 1000)
#endif

#endif /* _PORT_BSD_H */
