#include "port.h"
#include "port_bsd.h"
#include <stdint.h>
#include <string.h>
#include <sys/param.h>
#include <exec/errors.h>
#include <devices/scsidisk.h>
#include "scsipiconf.h"
#include "scsi_disk.h"
#include "scsipi_disk.h"
#include "scsipi_base.h"
#include "sd.h"
#include "attach.h"

int
diskstart(uint64_t blkno, uint b_flags, void *buf, uint buflen, void *ior)
{
    struct scsi_rw_6 cmd_small;
    struct scsipi_rw_10 cmd_big;
    struct scsipi_rw_16 cmd16;
    struct scsipi_generic *cmdp;
    struct scsipi_xfer *xs;
    struct scsipi_periph *periph = NULL;
    uint32_t nblks = buflen / 512;
    int cmdlen;
    int flags;

    printf("CDH: diskstart(%u, %u)\n", (uint32_t) blkno, nblks);

    extern struct scsipi_periph *global_periph;
    periph = global_periph;
    /*
     * Fill out the scsi command.  Use the smallest CDB possible
     * (6-byte, 10-byte, or 16-byte). If we need FUA or DPO,
     * need to use 10-byte or bigger, as the 6-byte doesn't support
     * the flags.
     */
    if (((blkno & 0x1fffff) == blkno) &&
        ((nblks & 0xff) == nblks)) {
        /* 6-byte CDB */
        memset(&cmd_small, 0, sizeof(cmd_small));
        cmd_small.opcode = (b_flags & B_READ) ? SCSI_READ_6_COMMAND :
                                                SCSI_WRITE_6_COMMAND;
        _lto3b(blkno, cmd_small.addr);
        cmd_small.length = nblks & 0xff;
        cmdlen = sizeof(cmd_small);
        cmdp = (struct scsipi_generic *)&cmd_small;
    } else if ((blkno & 0xffffffff) == blkno) {
        /* 10-byte CDB */
        memset(&cmd_big, 0, sizeof(cmd_big));
        cmd_big.opcode = (b_flags & B_READ) ? READ_10 : WRITE_10;
        _lto4b(blkno, cmd_big.addr);
        _lto2b(nblks, cmd_big.length);
        cmdlen = sizeof(cmd_big);
        cmdp = (struct scsipi_generic *)&cmd_big;
    } else {
        /* 16-byte CDB */
        memset(&cmd16, 0, sizeof(cmd16));
        cmd16.opcode = (b_flags & B_READ) ? READ_16 : WRITE_16;
        _lto8b(blkno, cmd16.addr);
        _lto4b(nblks, cmd16.length);
        cmdlen = sizeof(cmd16);
        cmdp = (struct scsipi_generic *)&cmd16;
    }
    flags = XS_CTL_NOSLEEP|XS_CTL_ASYNC|XS_CTL_SIMPLE_TAG;
    if (b_flags & B_READ)
        flags |= XS_CTL_DATA_IN;
    else
        flags |= XS_CTL_DATA_OUT;

    xs = scsipi_make_xs_locked(periph, cmdp, cmdlen, buf, buflen,
                               1, SD_IO_TIMEOUT, NULL, flags);
    xs->ior = ior;
    if (__predict_false(xs == NULL))
        return (1);  // out of memory

    printf("CDH: sd issue: %p\n", xs);
//  xs->
    return (scsipi_execute_xs(xs));
}

static const int8_t error_code_mapping[] = {
    0,                // XS_NOERROR           There is no error, (invalid sense)
    HFERR_BadStatus,  // XS_SENSE             Check returned sense for the error
    HFERR_BadStatus,  // XS_SHORTSENSE        Check ATAPI sense for the error
    HFERR_DMA,        // XS_DRIVER_STUFFUP    Driver failed operation
    HFERR_BadStatus,  // XS_RESOURCE_SHORTAGE Adapter resource shortage
    HFERR_SelTimeout, // XS_SELTIMEOUT        Device timed out.. turned off?
    HFERR_SelTimeout, // XS_TIMEOUT           Timeout was caught by SW
    IOERR_UNITBUSY,   // XS_BUSY              Device busy, try again later?
    HFERR_Phase,      // XS_RESET             Bus reset; possible retry command
    HFERR_Phase,      // XS_REQUEUE           Requeue this command
};

/* Called when a transfer is complete */
void
sd_complete(void *xsp, int rc)
{
    struct scsipi_xfer *xs = xsp;

    printf("CDH: sd complete: %p = %d\n", xs, rc);
    if (xs->ior == NULL) {
        printf("CDH: completion of NULL IOR\n");
        return;
    }
    if (rc != 0) {
        /* Translate error code to AmigaOS */
        scsipi_xfer_result_t res = xs->error;
        if (res < ARRAY_SIZE(error_code_mapping))
            rc = error_code_mapping[res];
    }
    amiga_sd_complete(xs->ior, rc);

    // XXX: Need to deallocate / free xs here
    // scsipi_put_xs(xs);
}
