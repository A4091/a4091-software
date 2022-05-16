#include "port.h"
#include "port_bsd.h"
#include <stdint.h>
#include <string.h>
#include <sys/param.h>
#include <exec/errors.h>
#include <devices/scsidisk.h>
#include <devices/trackdisk.h>
#include "scsipiconf.h"
#include "scsi_disk.h"
#include "scsipi_disk.h"
#include "scsipi_base.h"
#include "scsipi_all.h"
#include "siopreg.h"
#include "siopvar.h"
#include "sd.h"
#include "device.h"
#include "attach.h"
#include "cmdhandler.h"

#ifndef	SDRETRIES
#define	SDRETRIES 4
#endif

typedef struct
{
    struct scsi_mode_parameter_header_6 hdr;
    union scsi_disk_pages pg;
} scsi_mode_sense_t;

static void sd_complete(struct scsipi_xfer *xs);
static void scsidirect_complete(struct scsipi_xfer *xs);
static void geom_done_inquiry(struct scsipi_xfer *xs);

/*
 * sd_read_capacity
 * ----------------
 * Report device capacity in blocks and block size.
 */
static uint64_t
sd_read_capacity(struct scsipi_periph *periph, int *blksize, int flags)
{
    union {
        struct scsipi_read_capacity_10 cmd;
        struct scsipi_read_capacity_16 cmd16;
    } cmd;
    union {
        struct scsipi_read_capacity_10_data data;
        struct scsipi_read_capacity_16_data data16;
    } *datap;
    uint64_t capacity;

    memset(&cmd, 0, sizeof (cmd));
    cmd.cmd.opcode = READ_CAPACITY_10;

    /*
     * Don't allocate data buffer on stack;
     * The lower driver layer might use the same stack and
     * if it uses region which is in the same cacheline,
     * cache flush ops against the data buffer won't work properly.
     */
    datap = AllocMem(sizeof (*datap), MEMF_PUBLIC);
    if (datap == NULL)
        return 0;

    /*
     * If the command works, interpret the result as a 4 byte
     * number of blocks
     */
    capacity = 0;
    memset(datap, 0, sizeof(datap->data));
    if (scsipi_command(periph, (void *)&cmd.cmd, sizeof(cmd.cmd),
        (void *)datap, sizeof (datap->data), SCSIPIRETRIES, 20000, NULL,
        flags | XS_CTL_DATA_IN | XS_CTL_SILENT) != 0) {
        goto out;
    }

    if (_4btol(datap->data.addr) != 0xffffffff) {
        *blksize = _4btol(datap->data.length);
        capacity = _4btol(datap->data.addr) + 1;
        goto out;
    }

    /*
     * Device is larger than can be reflected by READ CAPACITY (10).
     * Try READ CAPACITY (16).
     */

    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd16.opcode = READ_CAPACITY_16;
    cmd.cmd16.byte2 = SRC16_SERVICE_ACTION;
    _lto4b(sizeof(datap->data16), cmd.cmd16.len);

    memset(datap, 0, sizeof(datap->data16));
    if (scsipi_command(periph, (void *)&cmd.cmd16, sizeof(cmd.cmd16),
        (void *)datap, sizeof(datap->data16), SCSIPIRETRIES, 20000, NULL,
        flags | XS_CTL_DATA_IN | XS_CTL_SILENT) != 0) {
        goto out;
    }

    *blksize = _4btol(datap->data16.length);
    capacity = _8btol(datap->data16.addr) + 1;

out:
    FreeMem(datap, sizeof (*datap));
    return (capacity);
}

/*
 * is_valid_blksize
 * ----------------
 * Returns non-zero if the specified device block size looks valid.
 * It must be a power of 2 and in the range of 256 to 32768 bytes.
 */
__attribute__((noinline))
static int8_t
is_valid_blksize(uint32_t blksize)
{
    return ((blksize & 0xff00) && ((blksize & (blksize - 1)) == 0));
}

static int
calc_blkshift(uint32_t blksize)
{
    uint shift = 0;
    for (shift = 0; blksize != 0; shift++)
        blksize >>= 1;

    return (shift - 1);
}

uint32_t
sd_blocksize(void *periph_p)
{
    struct scsipi_periph *periph = periph_p;
    int      flags;
    uint32_t blksize = 0;
    scsi_mode_sense_t modepage;

    if (periph->periph_blkshift != 0)
        return (1 << periph->periph_blkshift);

    flags = XS_CTL_SIMPLE_TAG | XS_CTL_DATA_IN;

    /*
     * SCSI Read Capacity can provide block size.
     */
    (void) sd_read_capacity(periph, &blksize, flags);
    if (is_valid_blksize(blksize))
        goto got_blocksize;

    /*
     * SCSI Mode page 3 can give bytes per sector and sectors per track
     */
    if (scsipi_mode_sense(periph, SMS_DBD, 3, &modepage.hdr,
                          sizeof (modepage.hdr) +
                          sizeof (modepage.pg.disk_format),
                          flags, SDRETRIES, 2000) == 0) {
        blksize = _2btol(modepage.pg.disk_format.bytes_s);
        if (is_valid_blksize(blksize))
            goto got_blocksize;
    }


    /*
     * SCSI Mode page 6 can give logical block size and number of logical
     * blocks.
     *
     * SMS_DBD tells mode sense to not return block descriptors.
     *
     * wcd BIT(0)=1 says drive will return GOOD only once all data
     * has landed on disk. BIT(0)=0 says drive may return GOOD even if
     * data has just landed in disk write cache.
     */
    if (scsipi_mode_sense(periph, SMS_DBD, 6, &modepage.hdr,
                          sizeof (modepage.hdr) +
                          sizeof (modepage.pg.rbc_params),
                          flags, SDRETRIES, 2000) == 0) {
        blksize = _2btol(modepage.pg.rbc_params.lbs);
        if (is_valid_blksize(blksize))
            goto got_blocksize;
    }

    blksize = TD_SECTOR; // Just give up and accept 512 as the default

got_blocksize:
    periph->periph_blkshift = calc_blkshift(blksize);
    return (blksize);
}

/*
 * sd_readwrite
 * ------------
 * Initiate a read or write operation on the specified SCSI device.
 * b_flags includes B_READ when the operation is a read from the SCSI
 * device to computer RAM.
 */
int
sd_readwrite(void *periph_p, uint64_t blkno, uint b_flags, void *buf,
             uint buflen, void *ior)
{
    struct scsipi_periph *periph = periph_p;
    struct scsipi_generic cmdbuf;
    struct scsipi_xfer *xs;
    uint32_t blkshift = periph->periph_blkshift;
    uint32_t nblks = buflen >> blkshift;
    int cmdlen;
    int flags;

    /*
     * Fill out the scsi command.  Use the smallest CDB possible
     * (6-byte, 10-byte, or 16-byte). If we need FUA or DPO,
     * need to use 10-byte or bigger, as the 6-byte doesn't support
     * the flags.
     */
    if (((blkno & 0x1fffff) == blkno) &&
        ((nblks & 0xff) == nblks)) {
        /* 6-byte CDB */
        struct scsi_rw_6 *cmd = (struct scsi_rw_6 *) &cmdbuf;
        cmdlen = sizeof (*cmd);
        memset(cmd, 0, cmdlen);

        cmd->opcode = (b_flags & B_READ) ? SCSI_READ_6_COMMAND :
                                           SCSI_WRITE_6_COMMAND;
        _lto3b(blkno, cmd->addr);
        cmd->length = nblks & 0xff;
    } else if ((blkno & 0xffffffff) == blkno) {
        /* 10-byte CDB */
        struct scsipi_rw_10 *cmd = (struct scsipi_rw_10 *) &cmdbuf;
        cmdlen = sizeof (*cmd);
        memset(cmd, 0, cmdlen);

        cmd->opcode = (b_flags & B_READ) ? READ_10 : WRITE_10;
        _lto4b(blkno, cmd->addr);
        _lto2b(nblks, cmd->length);
    } else {
        /* 16-byte CDB */
        struct scsipi_rw_16 *cmd = (struct scsipi_rw_16 *) &cmdbuf;
        cmdlen = sizeof (*cmd);
        memset(cmd, 0, cmdlen);

        cmd->opcode = (b_flags & B_READ) ? READ_16 : WRITE_16;
        _lto8b(blkno, cmd->addr);
        _lto4b(nblks, cmd->length);
    }
    flags = XS_CTL_ASYNC | XS_CTL_SIMPLE_TAG;
    if (b_flags & B_READ)
        flags |= XS_CTL_DATA_IN;
    else
        flags |= XS_CTL_DATA_OUT;

    xs = scsipi_make_xs_locked(periph, &cmdbuf, cmdlen, buf, buflen,
                               1, SD_IO_TIMEOUT, NULL, flags);
    if (__predict_false(xs == NULL))
        return (TDERR_NoMem);  // out of memory
    xs->amiga_ior = ior;
    xs->xs_done_callback = sd_complete;

    printf("sd%d.%d %p issue %c %u %u\n",
           periph->periph_target, periph->periph_lun, xs,
           (flags & XS_CTL_DATA_OUT) ? 'W' : 'R', (uint32_t) blkno, nblks);
    return (scsipi_execute_xs(xs));
}

#ifdef ENABLE_SEEK
/* Seek is implemented but untested code */
int
sd_seek(void *periph_p, uint64_t blkno, void *ior)
{
    struct scsipi_periph *periph = periph_p;
    struct scsipi_generic cmdbuf;
    struct scsipi_xfer *xs;
    int cmdlen;
    int flags;

    printf("seek(%u)\n", (uint32_t) blkno);

    /*
     * Fill out the scsi command.  Use the smallest CDB possible
     * (6-byte or 10-byte).
     */
    if ((blkno & 0x1fffff) == blkno) {
        /* 6-byte CDB */
        struct scsi_seek_6 *cmd = (struct scsi_seek_6 *) &cmdbuf;
        cmdlen = sizeof (*cmd);
        memset(cmd, 0, cmdlen);

        cmd->opcode = SCSI_SEEK_6_COMMAND;
        _lto3b(blkno, cmd->addr);
    } else if ((blkno & 0xffffffff) == blkno) {
        /* 10-byte CDB */
        struct scsi_seek_10 *cmd = (struct scsi_seek_10 *) &cmdbuf;
        cmdlen = sizeof (*cmd);
        memset(cmd, 0, cmdlen);

        cmd->opcode = SCSI_SEEK_10_COMMAND;
        _lto4b(blkno, cmd->addr);
    } else {
        return (1);
    }
    flags = XS_CTL_ASYNC | XS_CTL_SIMPLE_TAG;

    xs = scsipi_make_xs_locked(periph, &cmdbuf, cmdlen, NULL, 0,
                               1, SD_IO_TIMEOUT, NULL, flags);
    if (__predict_false(xs == NULL))
        return (TDERR_NoMem);  // out of memory
    xs->amiga_ior = ior;
    xs->xs_done_callback = sd_complete;

    printf("sd%d.%d %c %p issue\n",
           periph->periph_target, periph->periph_lun, 'S', xs);
    return (scsipi_execute_xs(xs));
}
#endif /* ENABLE_SEEK */

int
sd_getgeometry(void *periph_p, void *geom_p, void *ior)
{
    struct DriveGeometry *geom = geom_p;
    struct scsipi_periph *periph = periph_p;
    struct scsipi_inquiry_data *inq;
    struct scsipi_xfer *xs;
    struct scsipi_inquiry cmd;
    int flags = XS_CTL_ASYNC | XS_CTL_SIMPLE_TAG | XS_CTL_DATA_IN;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = INQUIRY;
    cmd.byte2 = periph->periph_lun << 5;
    cmd.unused[0] = 0;  /* Page Code */
    cmd.length = SCSIPI_INQUIRY_LENGTH_SCSI2;

    inq = AllocMem(sizeof (*inq), MEMF_PUBLIC);
    if (inq == NULL)
        return (TDERR_NoMem);

    geom->dg_SectorSize = 0;
    geom->dg_TotalSectors = 0;
    geom->dg_Cylinders = 0;
    geom->dg_Heads = 0;
    geom->dg_TrackSectors = 0;
    geom->dg_BufMemType = MEMF_PUBLIC;
    geom->dg_DeviceType = periph->periph_type & 0x1f;
    geom->dg_Flags = 0;
    geom->dg_Reserved = 0;

    xs = scsipi_make_xs_locked(periph,
                               (struct scsipi_generic *) &cmd, sizeof (cmd),
                               (uint8_t *) inq, sizeof (*inq),
                               1, 6000, NULL, flags);
    if (__predict_false(xs == NULL))
        return (1);  // out of memory
    xs->amiga_ior = ior;
    xs->xs_callback_arg = geom;
    xs->xs_done_callback = geom_done_inquiry;

    return (scsipi_execute_xs(xs));
}

/*
 * sd_get_protstatus
 * -----------------
 * Get disk write protect status (0=Not write protected)
 */
int
sd_get_protstatus(void *periph_p, unsigned long *status)
{
    struct scsipi_periph *periph = periph_p;
    int                   flags = XS_CTL_SIMPLE_TAG | XS_CTL_DATA_IN;
    int                   rc;
    scsi_mode_sense_t     modepage;

    if ((rc = scsipi_mode_sense(periph, SMS_DBD, 3, &modepage.hdr,
                          sizeof (modepage.hdr) +
                          sizeof (modepage.pg.control_params),
                          flags, SDRETRIES, 2000)) == 0) {
        *status = !!(modepage.pg.control_params.ctl_flags3 & CTL3_SWP);
        return (0);
    } else {
        /* Failure */
        *status = 0;
        return (rc);
    }
}

/*
 * sd_startstop
 * ------------
 * Send SCSI start or stop command (1=Start, 0=Stop)
 */
int
sd_startstop(void *periph_p, void *ior, int start, int load_eject)
{
    struct scsipi_periph *periph = periph_p;
    struct scsipi_xfer *xs;
    struct scsipi_start_stop cmd;
    int flags = XS_CTL_ASYNC | XS_CTL_SIMPLE_TAG;

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = START_STOP;
    cmd.byte2 = 0;
    cmd.how = 0;
    cmd.how = (start ? SSS_START : SSS_STOP) | (load_eject ? SSS_LOEJ : 0);

    xs = scsipi_make_xs_locked(periph, (struct scsipi_generic *) &cmd,
                               sizeof (cmd), NULL, 0, 1, SD_IO_TIMEOUT,
                               NULL, flags);
    if (__predict_false(xs == NULL))
        return (TDERR_NoMem);  // out of memory
    xs->amiga_ior = ior;
    xs->xs_done_callback = sd_complete;

    printf("sd%d.%d %p %s\n",
           periph->periph_target, periph->periph_lun, xs,
           load_eject ? (start ? "load" : "eject") :
           (start ? "start" : "stop"));
    return (scsipi_execute_xs(xs));
}


void
queue_get_mode_page(struct scsipi_xfer *oxs, uint8_t page, uint8_t dbd,
                    void *modepage_p, void (*done_cb)(struct scsipi_xfer *))
{
    struct scsipi_periph *periph = oxs->xs_periph;
    struct scsipi_xfer *xs;
    struct scsi_mode_sense_6 cmd;
    scsi_mode_sense_t *modepage = modepage_p;
    int flags;
    int rc;

    if (modepage == NULL) {
        modepage = AllocMem(sizeof (*modepage), MEMF_PUBLIC | MEMF_CLEAR);
        if (__predict_false(modepage == NULL)) {
            cmd_complete(oxs->amiga_ior, TDERR_NoMem);
            return;
        }
    }

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = SCSI_MODE_SENSE_6;
    cmd.byte2 = dbd;      // byte 2 DBD says to not return block descriptors.
    cmd.page = page;      // Which page to fetch
    cmd.length = 0x16;
//  cmd.length = sizeof (*modepage);

    flags = XS_CTL_ASYNC | XS_CTL_SIMPLE_TAG | XS_CTL_DATA_IN;
    xs = scsipi_make_xs_locked(periph,
                               (struct scsipi_generic *) &cmd, sizeof (cmd),
                               (uint8_t *)modepage, sizeof (*modepage),
                               1, 1000, NULL, flags);
    if (__predict_false(xs == NULL)) {
        FreeMem(modepage, sizeof (*modepage));
        cmd_complete(oxs->amiga_ior, 1);
        return;
    }
    xs->amiga_ior = oxs->amiga_ior;
    xs->xs_callback_arg = oxs->xs_callback_arg;
    xs->xs_done_callback = done_cb;

    rc = scsipi_execute_xs(xs);
    if (rc != 0) {
        FreeMem(modepage, sizeof (*modepage));
        cmd_complete(oxs->amiga_ior, rc);
    }
}

static void
geom_done_mode_page_5(struct scsipi_xfer *xs)
{
    int                rc       = xs->error;
    scsi_mode_sense_t *modepage = (scsi_mode_sense_t *) xs->data;
    printf("mode_page_5 complete: %d\n", rc);

#if 1
#ifndef NO_SERIAL_OUTPUT
{
    uint8_t *ptr = xs->data;
    int count = sizeof (*modepage);
    printf("GOT");
    while (count-- > 0)
        printf(" %02x", *(ptr++));
    printf("\n");
}
#endif
#endif
    if (rc == 0) {
        /* Got mode page 5 */
        struct DriveGeometry *geom = xs->xs_callback_arg;
        uint32_t nheads = modepage->pg.flex_geometry.nheads;
        uint32_t ncyl = _2btol(modepage->pg.flex_geometry.ncyl);
        uint32_t nspt = modepage->pg.flex_geometry.ph_sec_tr;  // sectors/track
        uint32_t blksize = _2btol(modepage->pg.flex_geometry.bytes_s);
        if (is_valid_blksize(blksize)) {
            struct scsipi_periph *periph = xs->xs_periph;
            geom->dg_SectorSize = blksize;
            periph->periph_blkshift = calc_blkshift(blksize);
        }
        if (nspt > 0)
            geom->dg_TrackSectors = nspt;
        if (ncyl > 0)
            geom->dg_Cylinders = ncyl;
        if (nheads > 0) {
            geom->dg_Heads = nheads;
            geom->dg_CylSectors = nheads * geom->dg_TrackSectors;
        }
        if (geom->dg_TotalSectors == 0)
            geom->dg_TotalSectors = geom->dg_TrackSectors * geom->dg_Heads *
                                    geom->dg_Cylinders;
        printf("final bs=%lu C=%lu H=%lu S=%lu Capacity=%lu\n",
               geom->dg_SectorSize, geom->dg_Cylinders, geom->dg_Heads,
               geom->dg_TrackSectors, geom->dg_TotalSectors);
    } else if (xs->cmdstore.bytes[0] == SMS_DBD) {
        /* Failed to get page. If DBD was on, try again with DBD off */
        queue_get_mode_page(xs, 5, 0, modepage, geom_done_mode_page_5);
        return;
    }
    FreeMem(modepage, sizeof (*modepage));
    cmd_complete(xs->amiga_ior, rc);
}

static void
geom_done_mode_page_4(struct scsipi_xfer *xs)
{
    int                rc       = xs->error;
    scsi_mode_sense_t *modepage = (scsi_mode_sense_t *) xs->data;
    printf("mode_page_4 complete: %d\n", rc);

    if (rc == 0) {
        /* Got mode page 4 */
        struct DriveGeometry *geom = xs->xs_callback_arg;
        uint32_t nheads = modepage->pg.rigid_geometry.nheads;
        uint32_t ncyl = _3btol(modepage->pg.rigid_geometry.ncyl);
        if (nheads > 0) {
            geom->dg_Heads = nheads;
            geom->dg_CylSectors = nheads * geom->dg_TrackSectors;
        }
        if (ncyl > 0)
            geom->dg_Cylinders = ncyl;
        if (geom->dg_TotalSectors == 0)
            geom->dg_TotalSectors = geom->dg_TrackSectors * geom->dg_Heads *
                                    geom->dg_Cylinders;
        printf("mode4 bs=%lu C=%lu H=%lu S=%lu Capacity=%lu\n",
               geom->dg_SectorSize, geom->dg_Cylinders, geom->dg_Heads,
               geom->dg_TrackSectors, geom->dg_TotalSectors);
    } else if (xs->cmdstore.bytes[0] == SMS_DBD) {
        /* Failed to get page. If DBD was on, try again with DBD off */
        queue_get_mode_page(xs, 4, 0, modepage, geom_done_mode_page_4);
        return;
    } else {
        /* Try mode page 5 */
        queue_get_mode_page(xs, 5, SMS_DBD, modepage, geom_done_mode_page_5);
        return;
    }

    FreeMem(modepage, sizeof (*modepage));
    cmd_complete(xs->amiga_ior, rc);
}

static void
geom_done_mode_page_3(struct scsipi_xfer *xs)
{
    int                rc       = xs->error;
    scsi_mode_sense_t *modepage = (scsi_mode_sense_t *) xs->data;

    if (rc == 0) {
        /* Got mode page 3 */
        struct DriveGeometry *geom = xs->xs_callback_arg;
        uint32_t blksize = _2btol(modepage->pg.disk_format.bytes_s);
        uint8_t  flags   = modepage->pg.disk_format.flags;
        if (is_valid_blksize(blksize)) {
            struct scsipi_periph *periph = xs->xs_periph;
            geom->dg_SectorSize = blksize;
            periph->periph_blkshift = calc_blkshift(blksize);
        }

        geom->dg_Flags |= (flags & DISK_FMT_RMB) ? DGF_REMOVABLE : 0;
        geom->dg_TrackSectors = _2btol(modepage->pg.disk_format.ph_sec_t);
        printf("mode_page_3 drive_type=%u ts=%lu blksize=%u\n",
               flags, geom->dg_TrackSectors, blksize);
    } else if (xs->cmdstore.bytes[0] == SMS_DBD) {
        /* Failed to get page. If DBD was on, try again with DBD off */
        queue_get_mode_page(xs, 3, 0, modepage, geom_done_mode_page_3);
        return;
    }

    queue_get_mode_page(xs, 4, SMS_DBD, modepage, geom_done_mode_page_4);
}

static void
conv_sectors_to_chs(ULONG total, ULONG *c_p, ULONG *h_p, ULONG *s_p)
{
    ULONG c = total / 2;
    ULONG h = 2;
    ULONG s = 1;
    while ((c >= 10000) && (h < 64) && (s < 32)) {
        c >>= 2;
        h <<= 1;
        s <<= 1;
    }
    *c_p = c;
    *h_p = h;
    *s_p = s;
}

static void
geom_done_get_capacity(struct scsipi_xfer *xs)
{
    if (xs->error == 0) {
        /*
         * Geometry struct was used as a holding buffer for TotalSectors
         * and SectorSize. SCSI has them in the opposite order of what
         * the Amiga struct specifies, so they must be swapped (below).
         * Also, SCSI READ_CAPACITY_10 reports the highest sector number,
         * and the dg_TotalSectors is the count of sectors, so need to
         * add 1 there.
         */
        struct DriveGeometry *geom = xs->xs_callback_arg;
        uint32_t blksize = geom->dg_TotalSectors;
        geom->dg_TotalSectors = geom->dg_SectorSize + 1;
        geom->dg_SectorSize = blksize;
        conv_sectors_to_chs(geom->dg_TotalSectors, &geom->dg_Cylinders,
                            &geom->dg_Heads, &geom->dg_TrackSectors);
        geom->dg_CylSectors = geom->dg_Heads * geom->dg_TrackSectors;
        if (is_valid_blksize(blksize)) {
            struct scsipi_periph *periph = xs->xs_periph;
            periph->periph_blkshift = calc_blkshift(blksize);
        }
        printf("TotalSectors=%lu C=%lu H=%lu S=%lu %p\n", geom->dg_TotalSectors,
               geom->dg_Cylinders, geom->dg_Heads, geom->dg_TrackSectors, xs);
        cmd_complete(xs->amiga_ior, 0);
        return;
    }

    /* Try a mode sense 3 / 4 combination */
    queue_get_mode_page(xs, 3, SMS_DBD, NULL, geom_done_mode_page_3);
}

static void
geom_done_inquiry(struct scsipi_xfer *oxs)
{
    struct scsipi_xfer *xs;
    struct DriveGeometry *geom = oxs->xs_callback_arg;
    struct scsipi_periph *periph = oxs->xs_periph;
    struct scsipi_inquiry_data *inq = (struct scsipi_inquiry_data *) oxs->data;
    struct scsipi_generic cmd;
    void                 *cap_buf;
    int rc = oxs->error;
    int cmd_len;
    int cap_len = 8;
    int flags = XS_CTL_ASYNC | XS_CTL_SIMPLE_TAG | XS_CTL_DATA_IN;

    if (rc == 0) {
        geom->dg_DeviceType = inq->device & 0x1f;
        geom->dg_Flags = (inq->dev_qual2 & SID_REMOVABLE) ? DGF_REMOVABLE : 0;
    }

    FreeMem(inq, sizeof (*inq));

    memset(&cmd, 0, sizeof (cmd));

    /*
     * Read capacity has number of blocks and block length
     *
     * READ_CAPACITY_10 0x25
     *   4 bytes block count
     *   4 bytes block size
     *     If block count == 0xffffffff, then issue READ_CAPACITY_16
     * READ_CAPACITY_16 0x9e
     *   8 bytes block count
     *   4 bytes block size
     */
    cmd.opcode = READ_CAPACITY_10;
    cmd_len = 10;  // Size of READ_CAPACITY_10

    cap_buf = &geom->dg_SectorSize;

    xs = scsipi_make_xs_locked(periph,
                               (struct scsipi_generic *) &cmd, cmd_len,
                               (uint8_t *) cap_buf, cap_len,
                               1, 1000, NULL, flags);
    xs->amiga_ior = oxs->amiga_ior;
    xs->xs_callback_arg = oxs->xs_callback_arg;
    xs->xs_done_callback = geom_done_get_capacity;

    rc = scsipi_execute_xs(xs);
    if (rc != 0)
        cmd_complete(oxs->amiga_ior, rc);
}

int
sd_scsidirect(void *periph_p, void *scmd_p, void *ior)
{
    int cmdlen;
    int flags;
    void *buf;
    uint buflen;
    struct scsipi_periph *periph = periph_p;
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
#if 0
// flags |= XS_CTL_DATA_UIO;
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
    xs->xs_callback_arg = scmd;
    xs->xs_done_callback = scsidirect_complete;
    printf("sdirect%d.%d %p issue\n",
           xs->xs_periph->periph_target, xs->xs_periph->periph_lun, xs);
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

/* Called when disk read/write transfer is complete */
static void
sd_complete(struct scsipi_xfer *xs)
{
    int rc = xs->error;

    if (xs->amiga_ior == NULL) {
        printf("NULL IOR received\n\n");
        return;  /* IOR was internally generated, not by user request */
    }

    printf("sd%d.%d %p done %c rc=%d\n",
           xs->xs_periph->periph_target, xs->xs_periph->periph_lun, xs,
           (xs->xs_control & XS_CTL_DATA_OUT) ? 'W' : 'R', rc);
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
        }
    }
    cmd_complete(xs->amiga_ior, rc);
}

static void
scsidirect_complete(struct scsipi_xfer *xs)
{
    int             rc   = xs->error;
    struct SCSICmd *scmd = xs->xs_callback_arg;

    scmd->scsi_Status    = rc;
    scmd->scsi_Actual    = scmd->scsi_Length;
    scmd->scsi_CmdActual = scmd->scsi_CmdLength;

    if (rc != 0) {
        printf("sdirect%d.%d fail %d\n",
               xs->xs_periph->periph_target, xs->xs_periph->periph_lun, rc);
        if (scmd->scsi_Flags & SCSIF_AUTOSENSE) {
            UWORD len = scmd->scsi_SenseLength;
            if (len < sizeof (xs->sense.scsi_sense))
                len = sizeof (xs->sense.scsi_sense);
            CopyMem(&xs->sense.scsi_sense, scmd->scsi_SenseData, len);
            scmd->scsi_SenseActual = len;
        }
    }
    cmd_complete(xs->amiga_ior, rc);
}
