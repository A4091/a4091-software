#ifdef DEBUG_CMDHANDLER
#define USE_SERIAL_OUTPUT
#endif
#include "port.h"
#include "printf.h"
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <libraries/expansionbase.h>
#include <devices/trackdisk.h>
#include <clib/expansion_protos.h>
#include <clib/exec_protos.h>
#include <clib/alib_protos.h>
#include <inline/expansion.h>
#include <exec/io.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <exec/execbase.h>
#include <exec/errors.h>
#include <exec/lists.h>
#include <dos/dostags.h>
#include <devices/scsidisk.h>

#include "device.h"
#include "scsi_all.h"
#include "scsipiconf.h"
#include "sd.h"
#include "sys_queue.h"
#include "siopreg.h"
#include "siopvar.h"
#include "attach.h"
#include "cmdhandler.h"
#include "nsd.h"
#include "ndkcompat.h"

#ifndef DEBUG_CMDHANDLER
#undef DEBUG_CMD
#endif

#ifdef DEBUG_CMD
#define PRINTF_CMD(args...) printf(args)
#else
#define PRINTF_CMD(args...)
#endif

#define BIT(x)        (1 << (x))

extern struct ExecBase *SysBase;
extern char real_device_name[];

a4091_save_t *asave = NULL;

/* Command handler startup structure */
typedef struct {
    struct MsgPort        *msg_port;  // Handler's message port (io_Unit)
    UBYTE                  boardnum;  // Desired board number   (io_Flags)
    BYTE                   io_Error;  // Success=0 or failure code
    struct SignalSemaphore started;   // Command handler has started
} start_msg_t;


void
irq_poll(uint got_int, struct siop_softc *sc)
{
    if (got_int) {
        siopintr(sc);
    } else if (sc->sc_flags & SIOP_INTSOFF) {
        /*
         * XXX: According to NCR 53C710-1 errata, polling ISTAT is not safe
         *      when a MODE is in flight to a register with carry. This is
         *      because during the ISTAT read, parity is briefly turned off.
         *      A possible work-around is to not check parity when reading
         *      the ISTAT register. Another possible work-around is to not
         *      poll the ISTAT register. Use the IRQ pin to determine when an
         *      interrupt has occurred. Once the interrupt has occurred, the
         *      ISTAT register can be read without any parity corruption.
         */
        siop_regmap_p rp    = sc->sc_siopp;
        uint8_t       istat = rp->siop_istat;

        if (istat & (SIOP_ISTAT_SIP | SIOP_ISTAT_DIP)) {
            sc->sc_istat = istat;
            sc->sc_sstat0 = rp->siop_sstat0;
            sc->sc_dstat  = rp->siop_dstat;
            siopintr(sc);
        }
    }
}

static void
restart_timer(void)
{
    if (asave->as_timerio[0] != NULL) {
        asave->as_timerio[0]->tr_time.tv_secs  = 1;
        asave->as_timerio[0]->tr_time.tv_micro = 0;
        asave->as_timerio[0]->tr_node.io_Command = TR_ADDREQUEST;
        SendIO(&asave->as_timerio[0]->tr_node);
        asave->as_timer_running = 1;
    }
}

static void
close_timer(void)
{
    int which;

    if (asave->as_timer_running) {
        WaitIO(&asave->as_timerio[0]->tr_node);
        asave->as_timer_running = 0;
    }

    for (which = 0; which < 2; which++) {
        if (asave->as_timerio[which] != NULL) {
            CloseDevice(&asave->as_timerio[which]->tr_node);
            DeleteExtIO(&asave->as_timerio[which]->tr_node);
            asave->as_timerio[which] = NULL;
        }

        if (asave->as_timerport[which] != NULL) {
            DeletePort(asave->as_timerport[which]);
            asave->as_timerport[which] = NULL;
        }
    }
}

static int
open_timer(void)
{
    int rc;
    int which;

    for (which = 0; which < 2; which++) {
        asave->as_timerport[which] = CreatePort(NULL, 0);
        if (asave->as_timerport[which] == NULL) {
            close_timer();
            return (ERROR_NO_MEMORY);
        }
        asave->as_timerio[which] = (struct timerequest *)
                                   CreateExtIO(asave->as_timerport[which],
                                               sizeof (struct timerequest));
        if (asave->as_timerio[which] == NULL) {
            printf("Fail: CreateExtIO timer\n");
            close_timer();
            return (ERROR_NO_MEMORY);
        }

        rc = OpenDevice(TIMERNAME, UNIT_VBLANK,
                        &asave->as_timerio[which]->tr_node, 0);
        if (rc != 0) {
            printf("Fail: open "TIMERNAME"\n");
            close_timer();
            return (rc);
        }
    }

    return (0);
}

void
cmd_complete(void *ior, int8_t rc)
{
    struct IOStdReq *ioreq = ior;

    if (ior == NULL) {
        printf("NULL ior in cmd_complete\n");
        return;
    }

    ioreq->io_Error = rc;
    ReplyMsg(&ioreq->io_Message);
}

#ifndef AddHeadMinList
void
AddHeadMinList(struct MinList *list, struct MinNode *node)
{
    struct MinNode *head;

    head            = list->mlh_Head;
    list->mlh_Head  = node;
    node->mln_Succ  = head;
    node->mln_Pred  = (struct MinNode *)list;
    head->mln_Pred  = node;
}
#endif

void
td_addchangeint(struct IORequest *ior)
{
    struct scsipi_periph *periph = (struct scsipi_periph *)ior->io_Unit;

    Forbid();
    AddHeadMinList(&periph->periph_changeintlist,
                   (struct MinNode *) &ior->io_Message.mn_Node);
    Permit();
    ior->io_Error = 0;  // Success
}

void
td_remchangeint(struct IORequest *ior)
{
    struct IORequest *io;
    struct scsipi_periph *periph = (struct scsipi_periph *)ior->io_Unit;

    Forbid();
    for (io = (struct IORequest *)periph->periph_changeintlist.mlh_Head;
         io->io_Message.mn_Node.ln_Succ != NULL;
         io = (struct IORequest *)io->io_Message.mn_Node.ln_Succ) {
        if (io == ior) {
            Remove(&io->io_Message.mn_Node);

            io->io_Message.mn_Node.ln_Succ = NULL;
            io->io_Message.mn_Node.ln_Pred = NULL;

            ior->io_Error = 0;
            break;
        }
    }
    Permit();
    if (io == NULL) {
        ior->io_Error = IOERR_BADADDRESS;
    } else {
        ior->io_Error = 0;  // Success
    }
}

void
td_remove(struct IORequest *ior)
{
    struct IOStdReq *io = (struct IOStdReq *) ior;
    struct scsipi_periph *periph = (struct scsipi_periph *)io->io_Unit;
    periph->periph_changeint = io->io_Data;
    ior->io_Error = 0;  // Success
}


static const UWORD nsd_supported_cmds[] = {
    CMD_READ, CMD_WRITE, TD_SEEK, TD_FORMAT,
    CMD_STOP, CMD_START,
    TD_GETGEOMETRY,
    TD_READ64, TD_WRITE64, TD_SEEK64, TD_FORMAT64,
    HD_SCSICMD,
    TD_PROTSTATUS, TD_CHANGENUM, TD_CHANGESTATE,
    NSCMD_DEVICEQUERY,
    NSCMD_TD_READ64, NSCMD_TD_WRITE64, NSCMD_TD_SEEK64, NSCMD_TD_FORMAT64,
    TAG_END
};

static int
cmd_do_iorequest(struct IORequest * ior)
{
    int             rc;
    uint64_t        blkno;
    uint            blkshift;
    struct IOExtTD *iotd = (struct IOExtTD *) ior;
    UWORD           cmd = ior->io_Command;

    ior->io_Error = 0;
    switch (cmd) {
        case ETD_WRITE:
        case ETD_READ:
        case ETD_MOTOR:
        case ETD_SEEK:
        case ETD_FORMAT:
            cmd &= ~TDF_EXTCOM;
            goto validate_etd;
        case NSCMD_ETD_READ64:
        case NSCMD_ETD_WRITE64:
        case NSCMD_ETD_SEEK64:
        case NSCMD_ETD_FORMAT64:
            cmd &= ~NSCMD_TDF_EXTCOM;
validate_etd:
        {
            struct scsipi_periph *periph = (struct scsipi_periph *)ior->io_Unit;
            if (periph->periph_changenum > iotd->iotd_Count) {
                /*
                 * Current change count is higher than maximum allowed
                 * change count. Reject the command.
                 */
                iotd->iotd_Req.io_Error = TDERR_DiskChanged;
                ReplyMsg(&iotd->iotd_Req.io_Message);
                return (0);
            }
            PRINTF_CMD("E");
            break;
        }
    }

    switch (cmd) {
        case CMD_READ:
            PRINTF_CMD("CMD_READ %d %"PRIx32" %"PRIx32"\n",
                    ((struct scsipi_periph *) ior->io_Unit)->periph_lun * 10 +
                    ((struct scsipi_periph *) ior->io_Unit)->periph_target,
                    iotd->iotd_Req.io_Offset, iotd->iotd_Req.io_Length);
            if (iotd->iotd_Req.io_Length == 0)
                goto io_done;
            blkshift = ((struct scsipi_periph *) ior->io_Unit)->periph_blkshift;
            blkno = iotd->iotd_Req.io_Offset >> blkshift;
CMD_READ_continue:
            rc = sd_readwrite(iotd->iotd_Req.io_Unit, blkno, B_READ,
                              iotd->iotd_Req.io_Data,
                              iotd->iotd_Req.io_Length, ior);
            if (rc == 0) {
                iotd->iotd_Req.io_Actual = iotd->iotd_Req.io_Length;
                /* cmd_complete() does ReplyMsg() */
            } else {
                iotd->iotd_Req.io_Error = rc;
io_done:
                iotd->iotd_Req.io_Actual = 0;
                ReplyMsg(&ior->io_Message);
            }
            break;

        case CMD_WRITE:
        case TD_FORMAT:
            PRINTF_CMD("CMD_WRITE %d %"PRIx32" %"PRIx32"\n",
                    ((struct scsipi_periph *) ior->io_Unit)->periph_lun * 10 +
                    ((struct scsipi_periph *) ior->io_Unit)->periph_target,
                    iotd->iotd_Req.io_Offset, iotd->iotd_Req.io_Length);
            if (iotd->iotd_Req.io_Length == 0)
                goto io_done;
            blkshift = ((struct scsipi_periph *) ior->io_Unit)->periph_blkshift;
            blkno = iotd->iotd_Req.io_Offset >> blkshift;
CMD_WRITE_continue:
            rc = sd_readwrite(iotd->iotd_Req.io_Unit, blkno, B_WRITE,
                              iotd->iotd_Req.io_Data,
                              iotd->iotd_Req.io_Length, ior);
            if (rc == 0) {
                iotd->iotd_Req.io_Actual = iotd->iotd_Req.io_Length;
                /* cmd_complete() does ReplyMsg() */
            } else {
                iotd->iotd_Req.io_Error = rc;
                iotd->iotd_Req.io_Actual = 0;
                ReplyMsg(&ior->io_Message);
            }
            break;

        case HD_SCSICMD:      // Send any SCSI command to drive (SCSI Direct)
#ifdef DEBUG_CMD
            {
                uint8_t *scmd = (uint8_t *) *(uint32_t *)
                                    (iotd->iotd_Req.io_Data + 12);
                PRINTF_CMD("HD_SCSICMD %d CMD=%02x %02x %02x %02x %02x %02x\n",
                    ((struct scsipi_periph *) ior->io_Unit)->periph_lun * 10 +
                    ((struct scsipi_periph *) ior->io_Unit)->periph_target,
                    scmd[0], scmd[1], scmd[2], scmd[3], scmd[4], scmd[5]);
            }
#endif
            rc = sd_scsidirect(iotd->iotd_Req.io_Unit,
                               iotd->iotd_Req.io_Data, ior);
            if (rc != 0) {
                iotd->iotd_Req.io_Error = rc;
                ReplyMsg(&ior->io_Message);
            }
            break;

        case NSCMD_TD_READ64:
            printf("NSCMD_");
        case TD_READ64:
            printf("TD64_READ %d %"PRIx32":%"PRIx32" %"PRIx32"\n",
                   ((struct scsipi_periph *) ior->io_Unit)->periph_lun * 10 +
                   ((struct scsipi_periph *) ior->io_Unit)->periph_target,
                   iotd->iotd_Req.io_Actual, iotd->iotd_Req.io_Offset,
                   iotd->iotd_Req.io_Length);
            if (iotd->iotd_Req.io_Length == 0)
                goto io_done;
            blkshift = ((struct scsipi_periph *) ior->io_Unit)->periph_blkshift;
            blkno = ((uint64_t) iotd->iotd_Req.io_Actual << (32 - blkshift)) |
                    (iotd->iotd_Req.io_Offset >> blkshift);
            goto CMD_READ_continue;

        case NSCMD_TD_FORMAT64:
        case NSCMD_TD_WRITE64:
            printf("NSCMD_");
        case TD_FORMAT64:
        case TD_WRITE64:
            printf("TD64_WRITE %d %"PRIx32":%"PRIx32" %"PRIx32"\n",
                   ((struct scsipi_periph *) ior->io_Unit)->periph_lun * 10 +
                   ((struct scsipi_periph *) ior->io_Unit)->periph_target,
                   iotd->iotd_Req.io_Actual, iotd->iotd_Req.io_Offset,
                   iotd->iotd_Req.io_Length);
            if (iotd->iotd_Req.io_Length == 0)
                goto io_done;
            blkshift = ((struct scsipi_periph *) ior->io_Unit)->periph_blkshift;
            blkno = ((uint64_t) iotd->iotd_Req.io_Actual << (32 - blkshift)) |
                    (iotd->iotd_Req.io_Offset >> blkshift);
            goto CMD_WRITE_continue;

#ifdef ENABLE_SEEK
        case NSCMD_TD_SEEK64:
        case TD_SEEK64:
            blkshift = ((struct scsipi_periph *) ior->io_Unit)->periph_blkshift;
            blkno = ((uint64_t) iotd->iotd_Req.io_Actual << (32 - blkshift)) |
                    (iotd->iotd_Req.io_Offset >> blkshift);
            goto CMD_SEEK_continue;
        case TD_SEEK:
            PRINTF_CMD("TD_SEEK %d off=%"PRIu32"\n",
                    ((struct scsipi_periph *) ior->io_Unit)->periph_lun * 10 +
                    ((struct scsipi_periph *) ior->io_Unit)->periph_target,
                    iotd->iotd_Req.io_Offset);
            blkshift = ((struct scsipi_periph *) ior->io_Unit)->periph_blkshift;
            blkno = iotd->iotd_Req.io_Offset >> blkshift;
CMD_SEEK_continue:
            rc = sd_seek(iotd->iotd_Req.io_Unit, blkno, ior);
            if (rc != 0) {
                iotd->iotd_Req.io_Error = rc;
                ReplyMsg(&ior->io_Message);
            }
            break;
#endif
        case TD_GETGEOMETRY:  // Get drive capacity, blocksize, etc
            PRINTF_CMD("TD_GETGEOMETRY %d\n",
                    ((struct scsipi_periph *) ior->io_Unit)->periph_lun * 10 +
                    ((struct scsipi_periph *) ior->io_Unit)->periph_target);
            rc = sd_getgeometry(iotd->iotd_Req.io_Unit,
                                iotd->iotd_Req.io_Data, ior);
            if (rc != 0) {
                iotd->iotd_Req.io_Error = rc;
                ReplyMsg(&ior->io_Message);
            }
            // TD_GETGEOMETRY without media should return TDERR_DiskChanged 29
            break;

#ifdef OLD_NSD_IDENTIFY
        /*
         * It is no longer recommended that TD_GETDRIVETYPE return
         * DRIVE_NEWSTYLE for NSD-style devices.
         */
        case TD_GETDRIVETYPE:
            iotd->iotd_Req.io_Actual = DRIVE_NEWSTYLE;
            break;
#endif

        case NSCMD_DEVICEQUERY: {
            struct NSDeviceQueryResult *nsd =
                (struct NSDeviceQueryResult *) iotd->iotd_Req.io_Data;
            if (iotd->iotd_Req.io_Length < 16) {
                ior->io_Error = ERROR_BAD_LENGTH;
            } else {
                nsd->DevQueryFormat      = 0;
                nsd->SizeAvailable       = sizeof (*nsd);
                nsd->DeviceType          = NSDEVTYPE_TRACKDISK;
                nsd->DeviceSubType       = 0;
                nsd->SupportedCommands   = (UWORD *) nsd_supported_cmds;
                iotd->iotd_Req.io_Actual = sizeof (*nsd);
            }
            ReplyMsg(&ior->io_Message);
            break;
        }

        case TD_PROTSTATUS:   // Is the disk write protected?
            PRINTF_CMD("TD_PROTSTATUS %d\n",
                    ((struct scsipi_periph *) ior->io_Unit)->periph_lun * 10 +
                    ((struct scsipi_periph *) ior->io_Unit)->periph_target);
            ior->io_Error = sd_get_protstatus(iotd->iotd_Req.io_Unit,
                                              &iotd->iotd_Req.io_Actual);
            ReplyMsg(&ior->io_Message);
            break;

        case TD_CHANGENUM:     // Number of disk changes
            PRINTF_CMD("TD_CHANGENUM %d\n",
                    ((struct scsipi_periph *) ior->io_Unit)->periph_lun * 10 +
                    ((struct scsipi_periph *) ior->io_Unit)->periph_target);
            iotd->iotd_Req.io_Actual =
                    ((struct scsipi_periph *) ior->io_Unit)->periph_changenum;
            ReplyMsg(&ior->io_Message);
            break;

        case TD_CHANGESTATE:   // Is there a disk in the drive?
            PRINTF_CMD("TD_CHANGESTATE %d\n",
                    ((struct scsipi_periph *) ior->io_Unit)->periph_lun * 10 +
                    ((struct scsipi_periph *) ior->io_Unit)->periph_target);
            rc = sd_testunitready(iotd->iotd_Req.io_Unit, ior);
            if (rc != 0) {
                iotd->iotd_Req.io_Error = rc;
                ReplyMsg(&ior->io_Message);
            }
            break;

        case CMD_STOP:         // Send SCSI STOP
            PRINTF_CMD("CMD_STOP %d\n",
                    ((struct scsipi_periph *) ior->io_Unit)->periph_lun * 10 +
                    ((struct scsipi_periph *) ior->io_Unit)->periph_target);
            rc = sd_startstop(iotd->iotd_Req.io_Unit, ior, 0, 0, 0);
            if (rc != 0) {
                iotd->iotd_Req.io_Error = rc;
                ReplyMsg(&ior->io_Message);
            }
            break;

        case CMD_START:        // Send SCSI START
            PRINTF_CMD("CMD_START %d\n",
                    ((struct scsipi_periph *) ior->io_Unit)->periph_lun * 10 +
                    ((struct scsipi_periph *) ior->io_Unit)->periph_target);
            rc = sd_startstop(iotd->iotd_Req.io_Unit, ior, 1, 0, 0);
            if (rc != 0) {
                iotd->iotd_Req.io_Error = rc;
                ReplyMsg(&ior->io_Message);
            }
            break;

        case TD_EJECT: {       // Eject/load physical media (typically CD-ROM)
            int load  = (iotd->iotd_Req.io_Length & ~2) == 0; // 1 = Eject
            int eject = (iotd->iotd_Req.io_Length & ~2) == 1;
            int immed = !!(iotd->iotd_Req.io_Length & 2);
            PRINTF_CMD("TD_EJECT %d %s\n",
                    ((struct scsipi_periph *) ior->io_Unit)->periph_lun * 10 +
                    ((struct scsipi_periph *) ior->io_Unit)->periph_target,
                    load ? "load" : eject ? "eject" : "invalid");

            if (load || eject)
                rc = sd_startstop(iotd->iotd_Req.io_Unit, ior, load, 1, immed);
            else
                rc = IOERR_BADLENGTH;

            if (rc != 0) {
                iotd->iotd_Req.io_Error = rc;
                ReplyMsg(&ior->io_Message);
            }
            break;
        }

        case CMD_ATTACH:  // Attach (open) a new SCSI device
            PRINTF_CMD("CMD_ATTACH %"PRIu32"\n", iotd->iotd_Req.io_Offset);

            rc = attach(&asave->as_device_self, iotd->iotd_Req.io_Offset,
                        (struct scsipi_periph **) &ior->io_Unit,
                        iotd->iotd_Req.io_Length);
            if (rc != 0) {
                ior->io_Error = rc;
            } else if ((iotd->iotd_Req.io_Length & TDF_DEBUG_OPEN) == 0) {
                (void) sd_blocksize((struct scsipi_periph *) ior->io_Unit);
            }

            ReplyMsg(&ior->io_Message);
            break;

        case CMD_DETACH:  // Detach (close) a SCSI device
            PRINTF_CMD("CMD_DETACH %d\n",
                   ((struct scsipi_periph *) ior->io_Unit)->periph_lun * 10 +
                   ((struct scsipi_periph *) ior->io_Unit)->periph_target);

            detach((struct scsipi_periph *) ior->io_Unit);
            ReplyMsg(&ior->io_Message);
            break;

        case CMD_TERM:
            PRINTF_CMD("CMD_TERM\n");
            deinit_chan(&asave->as_device_self);
            close_timer();
            asave->as_isr = NULL;
            FreeMem(asave->as_device_private, sizeof (*asave->as_device_private));
            FreeMem(asave, sizeof (*asave));
            asave = NULL;
            Forbid();
            DeletePort(myPort);
            myPort = NULL;
            ReplyMsg(&ior->io_Message);
            return (1);

        case TD_ADDCHANGEINT:  // TD_REMOVE done right
            PRINTF_CMD("TD_ADDCHANGEINT\n");
            td_addchangeint(ior);
            /* Do not reply to this request */
            break;

        case TD_REMCHANGEINT:  // Remove softint set by ADDCHANGEINT
            PRINTF_CMD("TD_REMCHANGEINT\n");
            td_remchangeint(ior);
            ReplyMsg(&ior->io_Message);
            break;

        case TD_REMOVE:        // Notify when media changes
            PRINTF_CMD("TD_REMOVE\n");
            td_remove(ior);
            ReplyMsg(&ior->io_Message);
            break;

        case TD_MOTOR:         // Turn the drive motor on or off
            /* Just reply with success, like the C= scsi.device does */
            ReplyMsg(&ior->io_Message);
            break;

        case CMD_INVALID:      // Invalid command (0)
        case CMD_RESET:        // Not supported by SCSI
        case CMD_UPDATE:       // Not supported by SCSI
        case CMD_CLEAR:        // Not supported by SCSI
        case CMD_FLUSH:        // Not supported by SCSI
        case TD_RAWREAD:       // Not supported by SCSI (raw bits from disk)
        case TD_RAWWRITE:      // Not supported by SCSI (raw bits to disk)
        case TD_GETNUMTRACKS:  // Not supported by SCSI (floppy-only)
        default:
            /* Unknown command */
            printf("Unknown cmd %x\n", ior->io_Command);
            ior->io_Error = ERROR_UNKNOWN_COMMAND;
            ReplyMsg(&ior->io_Message);
            break;
    }
    return (0);
}


void scsipi_completion_poll(struct scsipi_channel *chan);

/*
 * irq_and_timer_handler()
 * -----------------------
 * This function may be called by code which only needs to run background
 * processing of interrupts and timer events. The function will return
 * after each service action, so should be called repeatedly until an
 * external condition is met.
 */
int
irq_and_timer_handler(void)
{
    struct siop_softc     *sc   = asave->as_device_private;
    struct scsipi_channel *chan = &sc->sc_channel;
    uint32_t int_mask   = asave->as_int_mask;
    uint32_t timer_mask = asave->as_timer_mask;
    uint32_t mask;

    mask = Wait(int_mask | timer_mask);

    /* Handle incoming interrupts */
    irq_poll(mask & int_mask, sc);

    if (mask & timer_mask) {
        WaitIO(&asave->as_timerio[0]->tr_node);
        callout_run_timeouts();
        sd_testunitready_walk(chan);
        restart_timer();
    }

    /* Process the failure completion queue, if anything is present */
    scsipi_completion_poll(chan);
    return ((mask & int_mask) ? 1 : 0);
}

static void
cmd_handler(void)
{
    struct MsgPort        *msgport;
    struct IORequest      *ior;
    struct Task *task;
    struct siop_softc     *sc;
    struct scsipi_channel *chan;
    int                   *active;
    start_msg_t           *msg;
    ULONG                  int_mask;
    ULONG                  cmd_mask;
    ULONG                  wait_mask;
    ULONG                  timer_mask;
    uint32_t               mask;

    task = (struct Task *) FindTask((char *)NULL);

    msgport = CreatePort(NULL, 0);
    while ((msg = (start_msg_t *)(task->tc_UserData)) == NULL) {
        printf(".");
    }

    msg->msg_port = msgport;
    if (msgport == NULL) {
        msg->io_Error = ERROR_NO_MEMORY;
        goto fail_msgport;
    }

    asave = AllocMem(sizeof (*asave), MEMF_CLEAR | MEMF_PUBLIC);
    asave->as_device_private = AllocMem(sizeof (*asave->as_device_private),
                                        MEMF_CLEAR | MEMF_PUBLIC);

    if (asave == NULL) {
        msg->io_Error = ERROR_NO_MEMORY;
        goto fail_allocmem;
    }

    msg->io_Error = open_timer();
    if (msg->io_Error != 0)
        goto fail_timer;

    msg->io_Error = init_chan(&asave->as_device_self, &msg->boardnum);
    if (msg->io_Error != 0) {
        close_timer();
fail_timer:
        FreeMem(asave->as_device_private, sizeof (*asave->as_device_private));
        FreeMem(asave, sizeof (*asave));
fail_allocmem:
        /* Terminate handler and give up */
        DeletePort(msgport);
fail_msgport:
        ReleaseSemaphore(&msg->started);
        Forbid();
        return;
    }

    ReleaseSemaphore(&msg->started);
    restart_timer();

    sc         = asave->as_device_private;
    active     = &sc->sc_channel.chan_active;
    cmd_mask   = BIT(msgport->mp_SigBit);
    int_mask   = BIT(asave->as_irq_signal);
    timer_mask = BIT(asave->as_timerport[0]->mp_SigBit);
    wait_mask  = int_mask | timer_mask | cmd_mask;
    chan       = &sc->sc_channel;

    asave->as_int_mask   = int_mask;
    asave->as_timer_mask = timer_mask;

    while (1) {
        mask = Wait(wait_mask);

        if (asave->as_exiting)
            break;

        /* Handle incoming interrupts */
        do {
            irq_poll(mask & int_mask, sc);
        } while ((SetSignal(0, 0) & int_mask) && ((mask |= Wait(wait_mask))));

        /* Process timer events */
        if (mask & timer_mask) {
            WaitIO(&asave->as_timerio[0]->tr_node);
            callout_run_timeouts();
            sd_testunitready_walk(chan);
            restart_timer();
        }

        if (*active > 20) {
            wait_mask = int_mask | timer_mask;
            goto run_completion_queue;
        } else {
            wait_mask = int_mask | timer_mask | cmd_mask;
        }

        /* Handle new requests */
        while ((ior = (struct IORequest *)GetMsg(msgport)) != NULL) {
            if (cmd_do_iorequest(ior))
                return;  // Exit handler
            if (*active > 20) {
                wait_mask = int_mask | timer_mask;
                break;
            }
        }

        /* Process the failure completion queue, if anything is present */
run_completion_queue:
        scsipi_completion_poll(chan);
    }
}

int
start_cmd_handler(uint *boardnum)
{
    struct Task *task;
    start_msg_t msg;

    /* Prepare a startup structure with the board to initialize */
    memset(&msg, 0, sizeof (msg));
    msg.msg_port  = NULL;
    msg.boardnum  = *boardnum;
    msg.io_Error  = ERROR_OPEN_FAIL;  // Default, which should be overwritten
    InitSemaphore(&msg.started);
    ObtainSemaphore(&msg.started);

    task = CreateTask(real_device_name, 0, cmd_handler, 8192); // a4091.device
    if (task == NULL) {
        return (1);
    }
    msg.started.ss_Owner = task;    // Change ownership to created task
    task->tc_UserData = (APTR) &msg;

    ObtainSemaphore(&msg.started);  // Wait for task to release Semaphore

    myPort = msg.msg_port;
    *boardnum = msg.boardnum;

    return (msg.io_Error);
}

void
stop_cmd_handler(void)
{
    struct IORequest ior;

    memset(&ior, 0, sizeof (ior));
    ior.io_Message.mn_ReplyPort = CreateMsgPort();
    ior.io_Command = CMD_TERM;
    ior.io_Unit = NULL;
    PutMsg(myPort, &ior.io_Message);
    WaitPort(ior.io_Message.mn_ReplyPort);
    DeleteMsgPort(ior.io_Message.mn_ReplyPort);
}

typedef struct unit_list unit_list_t;

struct unit_list {
    unit_list_t          *next;
    struct scsipi_periph *periph;
    uint                  scsi_target;
    uint                  count;
};
unit_list_t *unit_list = NULL;

int
open_unit(uint scsi_target, void **io_Unit, uint flags)
{
    unit_list_t *cur;
    for (cur = unit_list; cur != NULL; cur = cur->next) {
        if (cur->scsi_target == scsi_target) {
            cur->count++;
            *io_Unit = cur->periph;
            return (0);
        }
    }
    if (flags & TDF_DEBUG_OPEN)
        return (ERROR_BAD_UNIT);  // This flag only grabs already open device

    cur = AllocMem(sizeof (*cur), MEMF_PUBLIC);
    if (cur == NULL)
        return (ERROR_NO_MEMORY);

    struct IOStdReq ior;
    ior.io_Message.mn_ReplyPort = CreateMsgPort();
    ior.io_Command = CMD_ATTACH;
    ior.io_Unit = NULL;
    ior.io_Offset = scsi_target;
    ior.io_Length = flags;

    PutMsg(myPort, &ior.io_Message);
    WaitPort(ior.io_Message.mn_ReplyPort);
    DeleteMsgPort(ior.io_Message.mn_ReplyPort);

    if (ior.io_Error != 0)
        return (ior.io_Error);

    *io_Unit = ior.io_Unit;
    if (ior.io_Unit == NULL)
        return (ERROR_BAD_UNIT);  // Attach failed

    /* Add new device to periph list */
    cur->count = 1;
    cur->periph = (struct scsipi_periph *) ior.io_Unit;
    cur->scsi_target = scsi_target;
    cur->next = unit_list;
    unit_list = cur;
    return (0);
}

void
close_unit(void *io_Unit)
{
    struct scsipi_periph *periph = io_Unit;
    unit_list_t *parent = NULL;
    unit_list_t *cur;
    for (cur = unit_list; cur != NULL; parent = cur, cur = cur->next) {
        if (cur->periph == periph) {
            if (--cur->count > 0)
                return;  // Peripheral is still open

            /* Remove device from list */
            if (parent == NULL)
                unit_list = cur->next;
            else
                parent->next = cur->next;
            FreeMem(cur, sizeof (*cur));

            /* Detach (close) peripheral */
            struct IOStdReq ior;
            ior.io_Message.mn_ReplyPort = CreateMsgPort();
            ior.io_Command = CMD_DETACH;
            ior.io_Unit = (struct Unit *) periph;

            PutMsg(myPort, &ior.io_Message);
            WaitPort(ior.io_Message.mn_ReplyPort);
            DeleteMsgPort(ior.io_Message.mn_ReplyPort);
            return;
        }
    }
    printf("Could not find unit %p to close\n", periph);
}
