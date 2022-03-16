#include "port.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#if 0
#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/device.h>
#include <sys/proc.h>
#endif

// #include "scsipi_all.h"
#include "scsipiconf.h"
#include "scsipi_base.h"

int
scsipi_command(struct scsipi_periph *periph, struct scsipi_generic *cmd,
    int cmdlen, u_char *data_addr, int datalen, int retries, int timeout,
    struct buf *bp, int flags)
{
        struct scsipi_xfer *xs;
        int rc;

        xs = scsipi_make_xs_locked(periph, cmd, cmdlen, data_addr, datalen,
            retries, timeout, bp, flags);
        if (!xs)
                return (ENOMEM);

        mutex_enter(chan_mtx(periph->periph_channel));
        rc = scsipi_execute_xs(xs);
        mutex_exit(chan_mtx(periph->periph_channel));

        return rc;
}
