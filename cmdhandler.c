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
// #include <inline/exec.h>
// #include <inline/dos.h>

#include "device.h"

#include "scsi_all.h"
#include "scsipiconf.h"
#include "sd.h"
#include "sys_queue.h"
#include "siopreg.h"
#include "siopvar.h"
#include "attach.h"
#include "cmdhandler.h"

#ifdef DEBUG_CMD
#define PRINTF_CMD(args...) printf(args)
#else
#define PRINTF_CMD(args...)
#endif

#define BIT(x)        (1 << (x))

extern struct ExecBase *SysBase;

#include "device.h"

a4091_save_t *asave = NULL;

typedef struct {
    struct Message msg;
    uint board;
    uint rc;
    a4091_save_t *drv_state;
} start_msg_t;


void
irq_poll(uint got_int, struct siop_softc *sc)
{
    if (sc->sc_flags & SIOP_INTSOFF) {
        siop_regmap_p rp    = sc->sc_siopp;
        uint8_t       istat = rp->siop_istat;

        if (istat & (SIOP_ISTAT_SIP | SIOP_ISTAT_DIP)) {
            sc->sc_istat = istat;
            sc->sc_sstat0 = rp->siop_sstat0;
            sc->sc_dstat  = rp->siop_dstat;
            siopintr(sc);
        }
    } else if (got_int) {
        siopintr(sc);
    }
}

static int
cmd_do_iorequest(struct IORequest * ior)
{
    int             rc;
    uint64_t        blkno;
    uint32_t        blksize;
    struct IOExtTD *iotd = (struct IOExtTD *) ior;

    ior->io_Error = 0;
    switch (ior->io_Command) {
        case CMD_READ:
            PRINTF_CMD("CMD_READ %lx %lx\n",
                       iotd->iotd_Req.io_Offset, iotd->iotd_Req.io_Length);
            blksize = ((struct scsipi_periph *) ior->io_Unit)->periph_blksize;
            blkno = iotd->iotd_Req.io_Offset / blksize;
            rc = sd_diskstart(iotd->iotd_Req.io_Unit, blkno, B_READ,
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

        case CMD_WRITE:
            PRINTF_CMD("CMD_WRITE %lx %lx\n",
                       iotd->iotd_Req.io_Offset, iotd->iotd_Req.io_Length);
            blksize = ((struct scsipi_periph *) ior->io_Unit)->periph_blksize;
            blkno = iotd->iotd_Req.io_Offset / blksize;
            rc = sd_diskstart(iotd->iotd_Req.io_Unit, blkno, B_WRITE,
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
/*
* ETD_READ
* ETD_WRITE
*/
        case HD_SCSICMD:      // Send any SCSI command to drive (SCSI Direct)
            rc = sd_scsidirect(iotd->iotd_Req.io_Unit,
                               iotd->iotd_Req.io_Data, ior);
            if (rc != 0) {
                iotd->iotd_Req.io_Error = rc;
                ReplyMsg(&ior->io_Message);
            }
            break;
        case TD_GETGEOMETRY:  // Get drive capacity, blocksize, etc
            rc = sd_getgeometry(iotd->iotd_Req.io_Unit,
                                iotd->iotd_Req.io_Data, ior);
            if (rc != 0) {
                iotd->iotd_Req.io_Error = rc;
                ReplyMsg(&ior->io_Message);
            }
            // TD_GETGEOMETRY without media should return TDERR_DiskChanged 29
            break;

        // CMD_INVALID
        // CMD_RESET
        // CMD_UPDATE
        // CMD_CLEAR
        // CMD_STOP
        // CMD_START
        // CMD_FLUSH
        //
        // ETD_WRITE
        // ETD_READ
        // ETD_MOTOR
        // ETD_SEEK
        // ETD_FORMAT
        // ETD_UPDATE
        // ETD_CLEAR
        // ETD_RAWREAD
        // ETD_RAWWRITE

        // TD_MOTOR          control the disk's motor
        // TD_SEEK           explicit seek (for testing)
        // TD_FORMAT         format disk
        // TD_REMOVE         notify when disk changes
        // TD_CHANGENUM      number of disk changes
        // TD_CHANGESTATE    is there a disk in the drive?
        // TD_PROTSTATUS     is the disk write protected?
        // TD_RAWREAD        read raw bits from the disk
        // TD_RAWWRITE       write raw bits to the disk
        // TD_GETDRIVETYPE   get the type of the disk drive
        // TD_GETNUMTRACKS   # of tracks for this type drive
        // TD_ADDCHANGEINT   TD_REMOVE done right
        // TD_REMCHANGEINT   remove softint set by ADDCHANGEINT
        // TD_EJECT          for those drives that support it

        case CMD_ATTACH:  // Attach (open) a new SCSI device
            PRINTF_CMD("CMD_ATTACH\n");
            rc = attach(&asave->as_device_self, iotd->iotd_Req.io_Offset,
                        (struct scsipi_periph **) &ior->io_Unit);
            if (rc != 0) {
                ior->io_Error = IOERR_OPENFAIL;
            } else {
                (void) sd_blocksize((struct scsipi_periph *) ior->io_Unit);
            }
            ReplyMsg(&ior->io_Message);
            break;

        case CMD_DETACH:  // Detach (close) a SCSI device
            PRINTF_CMD("CMD_DETACH\n");
            detach((struct scsipi_periph *) ior->io_Unit);
            ReplyMsg(&ior->io_Message);
            break;

        case CMD_TERM:
            PRINTF_CMD("CMD_TERM\n");
            deinit_chan(&asave->as_device_self);
            CloseLibrary((struct Library *) DOSBase);
            asave->as_isr = NULL;
            FreeMem(asave, sizeof (*asave));
            asave = NULL;
            DeletePort(myPort);
            myPort = NULL;
            Forbid();
            ReplyMsg(&ior->io_Message);
            return (1);

        default:
            /* Unknown command */
            printf("Unknown cmd %d\n", ior->io_Command);
            /* FALLTHROUGH */
        case TD_MOTOR:
            ior->io_Error = IOERR_NOCMD;
            ReplyMsg(&ior->io_Message);
            break;
    }
    return (0);
}

static void
cmd_handler(void)
{
    struct IORequest     *ior;
    struct Process       *proc;
    struct siop_softc    *sc;
    start_msg_t          *msg;
    ULONG                 int_mask;
    ULONG                 cmd_mask;
    ULONG                 wait_mask;
    uint                  board;
    uint32_t              mask;
#if 0
    register long devbase asm("a6");

    /* Builtin compiler function to set A4 to the global data area */
    geta4();

    devbase = msg->devbase;
    (void) devbase;
#endif

    proc = (struct Process *) FindTask((char *)NULL);

    /* get the startup message */
    while ((msg = (start_msg_t *) GetMsg(&proc->pr_MsgPort)) == NULL)
        WaitPort(&proc->pr_MsgPort);

    SysBase = *(struct ExecBase **) 4UL;
    DOSBase = (struct DosLibrary *) OpenLibrary("dos.library", 37L);
    board = msg->board;

    myPort = CreatePort(0, 0);
    if (myPort == NULL) {
        /* Terminate handler and give up */
        msg->rc = 1;
        ReplyMsg((struct Message *)msg);
        Forbid();
        return;
    }
    asave = msg->drv_state;
    msg->rc = init_chan(&asave->as_device_self, board);
    if (msg->rc != 0) {
        ReplyMsg((struct Message *)msg);
        Forbid();
        return;
    }

    ReplyMsg((struct Message *)msg);

    sc         = &asave->as_device_private;
    cmd_mask   = BIT(myPort->mp_SigBit);
    int_mask   = BIT(asave->as_irq_signal);
    wait_mask  = cmd_mask | int_mask;

    while (1) {
        mask = Wait(wait_mask);

        if (asave->as_exiting)
            break;

        do {
            irq_poll(mask & int_mask, sc);
        } while ((SetSignal(0, 0) & int_mask) && ((mask |= Wait(wait_mask))));

        if ((mask & cmd_mask) == 0)
            continue;

        while ((ior = (struct IORequest *)GetMsg(myPort)) != NULL) {
            if (cmd_do_iorequest(ior))
                return;  // Exit handler
        }
    }
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

int
start_cmd_handler(uint scsi_target, void *io_Unit)
{
    struct Process *myProc;
    struct DosLibrary *DOSBase;
    start_msg_t msg;
//    register long devbase asm("a6");
    a4091_save_t *drv_state;

    DOSBase = (struct DosLibrary *) OpenLibrary("dos.library", 37L);
    if (DOSBase == NULL)
        return (1);

    drv_state = AllocMem(sizeof (*drv_state), MEMF_CLEAR | MEMF_PUBLIC);
    if (drv_state == NULL) {
        printf("AllocMem failed\n");
        CloseLibrary((struct Library *) DOSBase);
        return (1);
    }

    myProc = CreateNewProcTags(NP_Entry, (ULONG) cmd_handler,
                               NP_StackSize, 8192,
                               NP_Priority, 0,
                               NP_Name, (ULONG) "A4091 bandler",
                               NP_CloseOutput, FALSE,
                               TAG_DONE);
    CloseLibrary((struct Library *) DOSBase);
    if (myProc == NULL)
        return (1);

    /* Send the startup message with the board to initialize */
    memset(&msg, 0, sizeof (msg));
    msg.msg.mn_Length = sizeof (start_msg_t) - sizeof (struct Message);
    msg.msg.mn_ReplyPort = CreatePort(0, 0);
    msg.msg.mn_Node.ln_Type = NT_MESSAGE;
    msg.board = scsi_target / 100;
    msg.drv_state = drv_state;
    PutMsg(&myProc->pr_MsgPort, (struct Message *)&msg);
    WaitPort(msg.msg.mn_ReplyPort);
    DeletePort(msg.msg.mn_ReplyPort);
    return (msg.rc);
}

void
stop_cmd_handler(void *io_Unit)
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
open_unit(uint scsi_target, void **io_Unit)
{
    unit_list_t *cur;
    for (cur = unit_list; cur != NULL; cur = cur->next) {
        if (cur->scsi_target == scsi_target) {
            cur->count++;
            *io_Unit = cur->periph;
            return (0);
        }
    }

    struct IOStdReq ior;
    ior.io_Message.mn_ReplyPort = CreateMsgPort();
    ior.io_Command = CMD_ATTACH;
    ior.io_Unit = NULL;
    ior.io_Offset = scsi_target;

    PutMsg(myPort, &ior.io_Message);
    WaitPort(ior.io_Message.mn_ReplyPort);
    DeleteMsgPort(ior.io_Message.mn_ReplyPort);
    if (ior.io_Error != 0)
        return (ior.io_Error);

    *io_Unit = ior.io_Unit;
    if (ior.io_Unit == NULL)
        return (1);  // Attach failed

    /* Add new device to periph list */
    cur = AllocMem(sizeof (*cur), MEMF_PUBLIC);
    if (cur == NULL) {
        FreeMem(cur, sizeof (*cur));
        return (1);
    }

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
