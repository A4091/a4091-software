#include "port.h"
#include "printf.h"
// // // // // // // // // #include <stdio.h>
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

#define ZORRO_MFG_COMMODORE     0x0202
#define ZORRO_PROD_A4091        0x0054

#define A4091_OFFSET_REGISTERS  0x00800000

#define A4091_INTPRI 30
#define A4091_IRQ    3

/* NCR53C710 registers */
#define REG_SCNTL0  0x03  // SCSI control 0
#define REG_SCNTL1  0x02  // SCSI control 1
#define REG_SDID    0x01  // SCSI destination ID
#define REG_SIEN    0x00  // SCSI interrupt enable
#define REG_SCID    0x07  // SCSI chip ID
#define REG_SCFER   0x06  // SCSI transfer
#define REG_SODL    0x05  // SCSI output data latch
#define REG_SOCL    0x04  // SCSI output control latch
#define REG_SFBR    0x0b  // SCSI first byte received
#define REG_SIDL    0x0a  // SCSI input data latch
#define REG_SBDL    0x09  // SCSI bus data lines
#define REG_SBCL    0x08  // SCSI bus control lines
#define REG_DSTAT   0x0f  // DMA status
#define REG_SSTAT0  0x0e  // SCSI status 0
#define REG_SSTAT1  0x0d  // SCSI status 1
#define REG_SSTAT2  0x0c  // SCSI status 2
#define REG_DSA     0x10  // Data structure address
#define REG_CTEST0  0x17  // Chip test 0
#define REG_CTEST1  0x16  // Chip test 1
#define REG_CTEST2  0x15  // Chip test 2
#define REG_CTEST3  0x14  // Chip test 3
#define REG_CTEST4  0x1b  // Chip test 4: MUX ZMOD SZM SLBE SFWR FBL2-FBL0
#define REG_CTEST5  0x1a  // Chip test 5
#define REG_CTEST6  0x19  // Chip test 6: DMA FIFO
#define REG_CTEST7  0x18  // Chip test 7
#define REG_TEMP    0x1c  // Temporary stack
#define REG_DFIFO   0x23  // DMA FIFO
#define REG_ISTAT   0x22  // Interrupt status
#define REG_CTEST8  0x21  // Chip test 8
#define REG_LCRC    0x20  // Longitudinal parity
#define REG_DBC     0x25  // DMA byte counter
#define REG_DCMD    0x24  // DMA command
#define REG_DNAD    0x28  // DMA next address for data
#define REG_DSP     0x2c  // DMA SCRIPTS pointer
#define REG_DSPS    0x30  // DMA SCRIPTS pointer save
#define REG_SCRATCH 0x34  // General purpose scratch pad
#define REG_DMODE   0x3b  // DMA mode
#define REG_DIEN    0x3a  // DMA interrupt enable
#define REG_DWT     0x39  // DMA watchdog timer
#define REG_DCNTL   0x38  // DMA control
#define REG_ADDER   0x3c  // Sum output of internal adder

#define REG_SIEN_PAR    BIT(0)  // Interrupt on parity error
#define REG_SIEN_RST    BIT(1)  // Interrupt on SCSI reset received
#define REG_SIEN_UDC    BIT(2)  // Interrupt on Unexpected disconnect
#define REG_SIEN_SGE    BIT(3)  // Interrupt on SCSI gross error
#define REG_SIEN_SEL    BIT(4)  // Interrupt on Selected or reselected
#define REG_SIEN_STO    BIT(5)  // Interrupt on SCSI bus timeout
#define REG_SIEN_FCMP   BIT(6)  // Interrupt on Function complete
#define REG_SIEN_PM     BIT(7)  // Interrupt on Unexpected Phase mismatch

#define REG_DIEN_BF     BIT(5)  // DMA interrupt on Bus Fault
#define REG_DIEN_ABRT   BIT(4)  // DMA interrupt on Aborted
#define REG_DIEN_SSI    BIT(3)  // DMA interrupt on SCRIPT Step Interrupt
#define REG_DIEN_SIR    BIT(2)  // DMA interrupt on SCRIPT Interrupt Instruction
#define REG_DIEN_WTD    BIT(1)  // DMA interrupt on Watchdog Timeout Detected
#define REG_DIEN_ILD    BIT(0)  // DMA interrupt on Illegal Instruction Detected

#define REG_ISTAT_DIP   BIT(0)  // DMA interrupt pending
#define REG_ISTAT_SIP   BIT(1)  // SCSI interrupt pending
#define REG_ISTAT_RST   BIT(6)  // Reset the 53C710
#define REG_ISTAT_ABRT  BIT(7)  // Abort

#define REG_DMODE_MAN   BIT(0)  // DMA Manual start mode
#define REG_DMODE_U0    BIT(1)  // DMA User programmable transfer type
#define REG_DMODE_FAM   BIT(2)  // DMA Fixed Address mode (set avoids DNAD inc)
#define REG_DMODE_PD    BIT(3)  // When set: FC0=0 for data & FC0=1 for program
#define REG_DMODE_FC1   BIT(4)  // Value driven on FC1 when bus mastering
#define REG_DMODE_FC2   BIT(5)  // Value driven on FC2 when bus mastering
#define REG_DMODE_BLE0  0                  // Burst length 1-transfer
#define REG_DMODE_BLE1  BIT(6)             // Burst length 2-transfers
#define REG_DMODE_BLE2  BIT(7)             // Burst length 4-transfers
#define REG_DMODE_BLE3  (BIT(6) | BIT(7))  // Burst length 8-transfers

#define REG_DCNTL_COM   BIT(0)  // Enable 53C710 mode
#define REG_DCNTL_SSM   BIT(4)  // SCRIPTS single-step mode
#define REG_DCNTL_EA    BIT(5)  // Enable Ack
#define REG_DCNTL_CFD0  BIT(7)             // SCLK 16.67-25.00 MHz
#define REG_DCNTL_CFD1  BIT(6)             // SCLK 25.01-37.50 MHz
#define REG_DCNTL_CFD2  0                  // SCLK 37.50-50.00 MHz
#define REG_DCNTL_CFD3  (BIT(7) | BIT(6))  // SCLK 50.01-66.67 MHz

#define REG_DSTAT_SSI   BIT(3)  // SCRIPTS single-step interrupt
#define REG_DSTAT_ABRT  BIT(4)  // SCRIPTS single-step interrupt
#define REG_DSTAT_DFE   BIT(7)  // DMA FIFO empty

#define REG_CTEST4_FBL2 BIT(2)  // Send CTEST6 register to lane of the DMA FIFO
#define REG_CTEST4_SLBE BIT(4)  // SCSI loopback mode enable

#define REG_CTEST4_CDIS BIT(7)  // Cache burst disable

#define ADDR8(x)      (volatile uint8_t *)(x)
#define ADDR32(x)     (volatile uint32_t *)(x)

#define BIT(x)        (1 << (x))

extern struct ExecBase *SysBase;

#include "device.h"

typedef struct {
    uint32_t              as_addr;
    struct DosLibrary    *as_DOSBase;
    struct ExecBase      *as_SysBase;
    uint32_t              as_irq_count;   // Total interrupts
    struct Task          *as_svc_task;
    struct Interrupt     *as_isr;         // My interrupt server
    uint8_t               as_irq_signal;
    volatile uint8_t      as_exiting;
    struct device         as_device_self;
//  struct scsipi_periph  as_periph;
    struct siop_softc     as_device_private;
} a4091_save_t;

a4091_save_t *asave = NULL;

typedef struct {
    struct Message msg;
    long devbase;
    uint scsi_target;
    uint rc;
    struct scsipi_periph *periph;
    a4091_save_t         *drv_state;
} start_msg_t;


void
irq_poll(uint got_int, struct siop_softc *sc)
{
    if (sc->sc_flags & SIOP_INTSOFF) {
        siop_regmap_p rp    = sc->sc_siopp;
        u_char        istat = rp->siop_istat;

        if (istat & (SIOP_ISTAT_SIP | SIOP_ISTAT_DIP)) {
            sc->sc_istat = istat;
            sc->sc_sstat0 = rp->siop_sstat0;
            sc->sc_dstat  = rp->siop_dstat;
printf("IP %02x\n", istat);
            siopintr(sc);
        }
    } else {
        siopintr(sc);
    }
}

void cmd_handler(void)
{
    struct IORequest     *ior;
    struct IOExtTD       *iotd;
    struct Process       *proc;
    struct siop_softc    *sc;
    start_msg_t          *msg;
    ULONG                 int_mask;
    ULONG                 cmd_mask;
    ULONG                 wait_mask;
    uint                  scsi_target;
    int                   rc;
    uint64_t              blkno;
    uint32_t              mask;
#if 0
    register long devbase asm("a6");
#endif

    proc = (struct Process *) FindTask((char *)NULL);
//  printf("cmd_handler()=%p\n", proc);

    /* get the startup message */
    while ((msg = (start_msg_t *) GetMsg(&proc->pr_MsgPort)) == NULL)
        WaitPort(&proc->pr_MsgPort);

#if 0
    /* Builtin compiler function to set A4 to the global data area */
    geta4();

    devbase = msg->devbase;
    (void) devbase;
#else
    SysBase = *(struct ExecBase **)4UL;
    DOSBase = (struct DosLibrary *) OpenLibrary("dos.library",37L);
#endif
    scsi_target = msg->scsi_target;

    myPort = CreatePort(0, 0);
    if (myPort == NULL) {
        /* Terminate handler and give up */
        msg->rc = 1;
        ReplyMsg((struct Message *)msg);
        Forbid();
        return;
    }
printf("a4091_save=%p %p periph=%p\n", asave, msg->drv_state, msg->periph);
    asave = msg->drv_state;
    msg->rc = attach(&asave->as_device_self, scsi_target, msg->periph);
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
//      WaitPort(myPort);
        mask = Wait(wait_mask);

        if (asave->as_exiting)
            break;
#if 0
        if (mask & int_mask)
            printf("Got INT BH\n");
#endif
        irq_poll(mask & int_mask, sc);

        if ((mask & cmd_mask) == 0)
            continue;

        while ((ior = (struct IORequest *)GetMsg(myPort)) != NULL) {
            ior->io_Error = 0;

            switch (ior->io_Command) {
                case CMD_TERM:
                    printf("CMD_TERM\n");
                    detach((struct scsipi_periph *) ior->io_Unit);
                    printf("Detach done %p\n", &asave->as_isr);
                    CloseLibrary((struct Library *) DOSBase);
                    asave->as_isr = NULL;
                    Forbid();
                    ReplyMsg(&ior->io_Message);
                    // DeletePort(myPort); ??  myPort = NULL;
                    return;

                case CMD_READ:
                    iotd = (struct IOExtTD *)ior;
                    printf("CMD_READ unit=%p %p\n", iotd->iotd_Req.io_Unit, ior->io_Unit);
                    printf("CMD_READ %lx %lx\n",
                           iotd->iotd_Req.io_Offset, iotd->iotd_Req.io_Length);
                    blkno = iotd->iotd_Req.io_Offset / 512;
                    rc = diskstart((struct scsipi_periph *)
                                   iotd->iotd_Req.io_Unit, blkno, B_READ,
                                   iotd->iotd_Req.io_Data,
                                   iotd->iotd_Req.io_Length, ior);
                    if (rc == 0) {
                        iotd->iotd_Req.io_Actual = iotd->iotd_Req.io_Length;
                        /* amiga_sd_complete() does ReplyMsg() */
                    } else {
                        iotd->iotd_Req.io_Error = rc;
                        iotd->iotd_Req.io_Actual = 0;
                        ReplyMsg(&ior->io_Message);
                    }
                    break;

                case CMD_WRITE:
                    iotd = (struct IOExtTD *)ior;
                    printf("CMD_WRITE %lx %lx\n",
                           iotd->iotd_Req.io_Offset, iotd->iotd_Req.io_Length);
#if 0
ior->io_Error = IOERR_NOCMD;
ReplyMsg(&ior->io_Message);
continue;
#endif
                    blkno = iotd->iotd_Req.io_Offset / 512;
                    rc = diskstart((struct scsipi_periph *)
                                   iotd->iotd_Req.io_Unit, blkno, B_READ,
                                   iotd->iotd_Req.io_Data,
                                   iotd->iotd_Req.io_Length, ior);
                    if (rc == 0) {
                        iotd->iotd_Req.io_Actual = iotd->iotd_Req.io_Length;
                        /* amiga_sd_complete() does ReplyMsg() */
                    } else {
                        iotd->iotd_Req.io_Error = rc;
                        iotd->iotd_Req.io_Actual = 0;
                        ReplyMsg(&ior->io_Message);
                    }
                    break;

/*
 * ETD_READ
 * ETD_WRITE
 * HD_SCSICMD = 28  (SCSI Direct)
 */
                case HD_SCSICMD:
                    iotd = (struct IOExtTD *)ior;
                    rc = scsidirect((struct scsipi_periph *)
                                    iotd->iotd_Req.io_Unit,
                                    iotd->iotd_Req.io_Data, ior);
                    if (rc != 0) {
                        iotd->iotd_Req.io_Error = rc;
                        ReplyMsg(&ior->io_Message);
                    }
                    break;
                case TD_GETGEOMETRY:
                    printf("TD_GETGEOMETRY not yet supportecsi\n");
                    ior->io_Error = IOERR_NOCMD;
                    ReplyMsg(&ior->io_Message);
                    break;

                default:
                    /* Unknown command */
                    printf("Unknown cmd %d\n", ior->io_Command);
                    ior->io_Error = IOERR_NOCMD;
                    ReplyMsg(&ior->io_Message);
                    break;
            }
        }
    }
}

void
amiga_sd_complete(void *ior, int8_t rc)
{
    struct IOStdReq *ioreq = ior;

    ioreq->io_Error = rc;
    ReplyMsg(&ioreq->io_Message);
}

int
create_cmd_handler(uint scsi_target, void *io_Unit)
{
    struct Process *myProc;
    start_msg_t msg;
    register long devbase asm("a6");
    a4091_save_t *drv_state;

    DOSBase = (struct DosLibrary *) OpenLibrary("dos.library",37L);
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
                               NP_Name, (ULONG) "CMD_Handler",
                               NP_CloseOutput, FALSE,
                               TAG_DONE);
    CloseLibrary((struct Library *) DOSBase);
    if (myProc == NULL)
        return (1);

    /* Send the startup message with the library base pointer */
    msg.msg.mn_Length = sizeof (start_msg_t) - sizeof (struct Message);
    msg.msg.mn_ReplyPort = CreatePort(0,0);
    msg.msg.mn_Node.ln_Type = NT_MESSAGE;
    msg.devbase = devbase;
    msg.scsi_target = scsi_target;
    msg.periph = io_Unit;
    msg.drv_state = drv_state;
    PutMsg(&myProc->pr_MsgPort, (struct Message *)&msg);
    WaitPort(msg.msg.mn_ReplyPort);
    DeletePort(msg.msg.mn_ReplyPort);

    if (msg.rc != 0)  /* Handler failed to start */
        return (1);

    return (0);
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
    struct scsipi_periph *periph;
    unit_list_t *cur;
    for (cur = unit_list; cur != NULL; cur = cur->next) {
        if (cur->scsi_target == scsi_target) {
            cur->count++;
            *io_Unit = cur->periph;
            return (0);
        }
    }
    periph = AllocMem(sizeof (*periph), MEMF_PUBLIC | MEMF_CLEAR);
    if (periph == NULL)
        return (1);

    cur = AllocMem(sizeof (*cur), MEMF_PUBLIC);
    if (cur == NULL) {
        FreeMem(cur, sizeof (*cur));
        return (1);
    }

    cur->count = 1;
    cur->periph = periph;
    cur->scsi_target = scsi_target;
    cur->next = unit_list;
    unit_list = cur;

    *io_Unit = periph;
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
            if (--cur->count == 0) {
                if (parent == NULL)
                    unit_list = cur->next;
                else
                    parent->next = cur->next;
                FreeMem(cur, sizeof (*cur));
            }
            return;
        }
    }
    printf("Could not find unit %p to close\n", periph);
}
