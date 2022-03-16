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
#include "cmdhandler.h"

int
diskstart(struct scsipi_periph *periph, uint64_t blkno, uint b_flags, void *buf, uint buflen, void *ior)
{
    struct scsi_rw_6 cmd_small;
    struct scsipi_rw_10 cmd_big;
    struct scsipi_rw_16 cmd16;
    struct scsipi_generic *cmdp;
    struct scsipi_xfer *xs;
//    struct scsipi_periph *periph = NULL;
    uint32_t nblks = buflen / 512;
    int cmdlen;
    int flags;

    printf("CDH: diskstart(%u, %u, %c)\n", (uint32_t) blkno, nblks, (b_flags & B_READ) ? 'R' : 'W');

#if 0
    extern struct scsipi_periph *global_periph;
    periph = global_periph;
#endif
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
    flags = XS_CTL_ASYNC | XS_CTL_SIMPLE_TAG;
    if (b_flags & B_READ)
        flags |= XS_CTL_DATA_IN;
    else
        flags |= XS_CTL_DATA_OUT;

    xs = scsipi_make_xs_locked(periph, cmdp, cmdlen, buf, buflen,
                               1, SD_IO_TIMEOUT, NULL, flags);
    if (__predict_false(xs == NULL))
        return (1);  // out of memory
    xs->amiga_ior = ior;

    printf("CDH: sd issue: %p\n", xs);
    return (scsipi_execute_xs(xs));
}

int
scsidirect(struct scsipi_periph *periph, void *scmd_p, void *ior)
{
    int cmdlen;
    int flags;
    void *buf;
    uint buflen;
    struct scsipi_xfer *xs;
    struct scsipi_generic *cmdp;
    struct SCSICmd *scmd = scmd_p;
    printf("scsidirect dlen=%lu clen=%u slen=%u flags=%x [",
           scmd->scsi_Length, scmd->scsi_CmdLength, scmd->scsi_SenseLength,
           scmd->scsi_Flags);

    for (cmdlen = 0; cmdlen < scmd->scsi_CmdLength; cmdlen++) {
        if (cmdlen > 0)
            putchar(' ');
        printf("%02x", scmd->scsi_Command[cmdlen]);
    }
    printf("]\n");

    scmd->scsi_Status = HFERR_BadStatus;  // XXX might need to be set later
    scmd->scsi_Status = 0;
    scmd->scsi_Actual = 0;          /* Actual data used */
    scmd->scsi_CmdActual = 0;       /* Actual command used */
    scmd->scsi_SenseActual = 0;     /* Actual sense data returned */

    cmdlen = scmd->scsi_CmdLength;
    cmdp = (struct scsipi_generic *) scmd->scsi_Command;

    buf = scmd->scsi_Data;
    buflen = scmd->scsi_Length;

    flags = XS_CTL_ASYNC | XS_CTL_SIMPLE_TAG;
    if (scmd->scsi_Flags & SCSIF_READ)
        flags |= XS_CTL_DATA_IN;
    else
        flags |= XS_CTL_DATA_OUT;
// flags |= XS_CTL_DATA_UIO;
#if 0
    if ((scmd->scsi_Flags & SCSIF_OLDAUTOSENSE) == SCSIF_OLDAUTOSENSE)
        printf("scsidirect: Old autosense\n");
    else if (scmd->scsi_Flags & SCSIF_AUTOSENSE)
        printf("scsidirect: Autosense\n");
#endif

    xs = scsipi_make_xs_locked(periph, cmdp, cmdlen, buf, buflen,
                               1, SD_IO_TIMEOUT, NULL, flags);
    if (__predict_false(xs == NULL))
        return (1);  // out of memory
    xs->amiga_ior = ior;
    xs->amiga_sdirect = scmd;

    printf("CDH: sdirect issue: %p\n", xs);
    return (scsipi_execute_xs(xs));
// xs->xs_control |= XS_CTL_USERCMD;  // to indicate user command
//
// flags |= XS_CTL_DATA_UIO;
// XS_CTL_RESET XS_CTL_TARGET XS_CTL_ESCAPE XS_CTL_URGENT
//   XS_CTL_SIMPLE_TAG XS_CTL_ORDERED_TAG XS_CTL_HEAD_TAG
//   XS_CTL_REQSENSE
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

#if 0
static void
request_sense(struct scsipi_xfer *xs)
{
    struct scsi_request_sense cmd;
#if 0
    struct scsipi_xfer *xs;
    int flags = oxs->xs_control;

    flags |= XS_CTL_REQSENSE | XS_CTL_URGENT | XS_CTL_DATA_IN |
             XS_CTL_THAW_PERIPH | XS_CTL_FREEZE_PERIPH;

    xs = scsipi_make_xs_locked(oxs->periph, cmdp, cmdlen,
                               (void *)&xs->sense.scsi_sense,
                               sizeof (struct scsi_sense_data),
                               1, SD_IO_TIMEOUT, NULL, flags);
    if (__predict_false(xs == NULL))
        return;  // out of memory
    xs->amiga_ior = ior;
#else
    xs->xs_control |= XS_CTL_REQSENSE | XS_CTL_URGENT | XS_CTL_DATA_IN |
                      XS_CTL_THAW_PERIPH | XS_CTL_FREEZE_PERIPH;
    xs->xs_status = 0;

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = SCSI_REQUEST_SENSE;
    cmd.length = sizeof (struct scsi_sense_data);
    xs->cmd = (void *)&cmd;
    xs->cmdlen = cmd.length;
    xs->data = (void *)&xs->sense.scsi_sense;
    xs->datalen = sizeof (struct scsi_sense_data);
    xs->xs_retries = 0;
    xs->timeout = 1000;
    xs->bp = NULL;
#endif

    printf("CDH: request_sense %p ior=%p\n", xs, xs->amiga_ior);
    scsipi_execute_xs(xs);
}
#endif

/* Called when a transfer is complete */
void
sd_complete(void *xsp, int rc)
{
    struct scsipi_xfer *xs = xsp;

    printf("CDH: sd complete: %p rc=%d\n", xs, rc);
    if (xs->amiga_ior == NULL) {
        printf("CDH: completion of NULL IOR\n");
        return;
    }
    if (xs->amiga_sdirect != NULL) {
        struct SCSICmd *scmd = xs->amiga_sdirect;
        scmd->scsi_Status    = rc;
        scmd->scsi_Actual    = scmd->scsi_Length;
        scmd->scsi_CmdActual = scmd->scsi_CmdLength;
    }
    if (rc != 0) {
        /* Translate error code to AmigaOS */
        scsipi_xfer_result_t res = xs->error;
        if (res < ARRAY_SIZE(error_code_mapping))
            rc = error_code_mapping[res];

        if (xs->error == XS_SENSE) {
            printf("got sense:");
            for (int t = 0; t < sizeof (xs->sense.scsi_sense); t++)
                printf(" %02x", ((uint8_t *) &xs->sense.scsi_sense)[t]);
            printf("\n");
            if (xs->amiga_sdirect != NULL) {
                struct SCSICmd *scmd = xs->amiga_sdirect;
                if (scmd->scsi_Flags & SCSIF_AUTOSENSE) {
                    UWORD len = scmd->scsi_SenseLength;
                    if (len < sizeof (xs->sense.scsi_sense))
                        len = sizeof (xs->sense.scsi_sense);
                    memcpy(scmd->scsi_SenseData, &xs->sense.scsi_sense, len);
                    scmd->scsi_SenseActual = len;
                }
            }
        }
    }
    amiga_sd_complete(xs->amiga_ior, rc);

    // XXX: Need to deallocate / free xs here
    // scsipi_put_xs(xs);
}
