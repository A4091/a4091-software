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
// #include <inline/exec.h>
// #include <inline/dos.h>

#if 0
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/device.h>
#endif
#include "device.h"

#include "scsi_all.h"
#include "scsipiconf.h"
#include "sd.h"
#if 0
#include <dev/scsipi/scsi_all.h>
#include <dev/scsipi/scsipi_all.h>
#include <dev/scsipi/scsiconf.h>
#include <machine/cpu.h>
#include <amiga/amiga/custom.h>
#include <amiga/amiga/cc.h>
#include <amiga/amiga/device.h>
#include <amiga/amiga/isr.h>
#include <amiga/dev/siopreg.h>
#include <amiga/dev/siopvar.h>
#include <amiga/dev/zbusvar.h>
#endif
#include "sys_queue.h"
#include "siopreg.h"
#include "siopvar.h"
#include "port_bsd.h"
#include "attach.h"

#ifdef __powerpc__
#define badaddr(a)      badaddr_read(a, 2, NULL)
#endif

// int afscmatch(device_t, cfdata_t, void *);
#ifdef DEBUG
void afsc_dump(void);
#endif

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

typedef struct {
    struct Message msg;
    long devbase;
    uint which_target;
    uint rc;
} start_msg_t;

static int a4091_add_local_irq_handler(uint32_t dev_base);
static const char * const expansion_library_name = "expansion.library";

extern struct ExecBase *SysBase;

#include "device.h"
static struct device device_self;
struct scsipi_periph *global_periph;

#include "glob.h"
a4091_save_t a4091_save;

#if 1
static uint8_t
get_ncrreg8(uint32_t a4091_base, uint reg)
{
    return (*ADDR8(a4091_base + A4091_OFFSET_REGISTERS + reg));
}
#else
static uint8_t
get_ncrreg8(uint reg)
{
    return (*ADDR8(a4091_base + A4091_OFFSET_REGISTERS + reg));
}
#endif

#if 0
static uint32_t
get_ncrreg32(uint reg)
{
    return (*ADDR32(a4091_base + A4091_OFFSET_REGISTERS + reg));
}
#endif

#if 1
static void
set_ncrreg8(uint32_t a4091_base, uint reg, uint8_t value)
{
    *ADDR8(a4091_base + A4091_OFFSET_REGISTERS + reg) = value;
}
#else
static void
set_ncrreg8(uint reg, uint8_t value)
{
    *ADDR8(a4091_base + A4091_OFFSET_REGISTERS + reg) = value;
}
#endif

#if 0
static void
set_ncrreg32(uint reg, uint32_t value)
{
    *ADDR32(a4091_base + A4091_OFFSET_REGISTERS + reg) = value;
}
#endif

/*
 * a4091_reset
 * -----------
 * Resets the A4091's 53C710 SCSI controller.
 */
static void
a4091_reset(uint32_t dev_base)
{
    /* Enable Ack: allow reg. writes */
    set_ncrreg8(dev_base, REG_DCNTL, REG_DCNTL_EA);

    /* Reset */
    set_ncrreg8(dev_base, REG_ISTAT, REG_ISTAT_RST);

    (void) get_ncrreg8(dev_base, REG_ISTAT); // Push out write

    /* Clear reset */
    set_ncrreg8(dev_base, REG_ISTAT, 0);
    (void) get_ncrreg8(dev_base, REG_ISTAT); // Push out write

    /* Set SCSI ID */
    set_ncrreg8(dev_base, REG_SCID, BIT(7));

    /* SCSI Core clock (37.51-50 MHz) */
    set_ncrreg8(dev_base, REG_DCNTL, REG_DCNTL_EA);

#if 0
    /* Set DMA interrupt enable on Bus Fault, Abort, or Illegal instruction */
    set_ncrreg8(dev_base, REG_DIEN, REG_DIEN_BF | REG_DIEN_ABRT | REG_DIEN_ILD);
#endif

    // Reset Enable Acknowlege and Function Control One (FC1) bits?
}

/*
 * irq_handler_core
 * ----------------
 * This is the actual interrupt handler. It checks whether the interrupt
 * register of the SCSI chip indicates there is something to do, and if
 * so also captures the SCSI Status 0 and DMA Status, and then wakes the
 * service task to go process them.
 */
__attribute__((noinline))
static void
irq_handler_core(a4091_save_t *save)
{
    struct siop_softc *sc = save->as_device_private;
    siop_regmap_p      rp;
    u_char             istat;

    if (sc->sc_flags & SIOP_INTSOFF)
        return; /* interrupts are not active */

    rp = sc->sc_siopp;
    istat = rp->siop_istat;
    if ((istat & (SIOP_ISTAT_SIP | SIOP_ISTAT_DIP)) == 0)
        return;
    save->as_irq_count++;

    /*
     * save Interrupt Status, SCSI Status 0, and DMA Status.
     * (may need to deal with stacked interrupts?)
     */
    sc->sc_istat  = istat;
    sc->sc_sstat0 = rp->siop_sstat0;
    sc->sc_dstat  = rp->siop_dstat;

    if (save->as_svc_task != NULL)
        Signal(save->as_svc_task, BIT(save->as_irq_signal));
}

/*
 * irq_handler
 * -----------
 * Simple trampoline function to ensure that the last statement
 * sets the Z flag appropriately. It would be best to rewrite
 * this code in assembly to reduce instruction count. Maybe change
 * to "tst d0" if the called function wants to return anything
 * other than 0.
 */
LONG
irq_handler(void)
{
    register a4091_save_t *save asm("a1");
    irq_handler_core(save);
    return (0);
}

void
irq_poll(uint got_int)
{
    struct siop_softc *sc = a4091_save.as_device_private;
    if (sc->sc_flags & SIOP_INTSOFF) {
        siop_regmap_p rp    = sc->sc_siopp;
        u_char        istat = rp->siop_istat;

        if (istat & (SIOP_ISTAT_SIP | SIOP_ISTAT_DIP)) {
            sc->sc_istat = istat;
            sc->sc_sstat0 = rp->siop_sstat0;
            sc->sc_dstat  = rp->siop_dstat;
printf("IP %02x\n", istat);
            siopintr(sc);
        } else {
printf("IP nothing\n");
        }
    } else {
        siopintr(sc);
    }
}

static int
a4091_add_local_irq_handler(uint32_t dev_base)
{
    struct Task *task = FindTask(NULL);
    if (task == NULL)
        return (0);
#if 0
    save = (a4091_save_t *) ((struct Process *) task)->pr_ExitData;
    if (save == NULL)
        return (0);
#endif

    a4091_save.as_addr           = dev_base;
    a4091_save.as_SysBase        = SysBase;
    a4091_save.as_DOSBase        = DOSBase;
    a4091_save.as_irq_count      = 0;
    a4091_save.as_device_private = device_private(NULL);
    a4091_save.as_svc_task       = task;
    a4091_save.as_irq_signal     = AllocSignal(-1);
    a4091_save.as_isr            = AllocMem(sizeof (*a4091_save.as_isr),
                                            MEMF_CLEAR | MEMF_PUBLIC);
    if (a4091_save.as_isr == NULL) {
        printf("AllocMem failed\n");
        return (1);
    }

    a4091_save.as_isr->is_Node.ln_Type = NT_INTERRUPT;
    a4091_save.as_isr->is_Node.ln_Pri  = A4091_INTPRI;
    a4091_save.as_isr->is_Node.ln_Name = "A4091.device";
    a4091_save.as_isr->is_Data         = &a4091_save;
    a4091_save.as_isr->is_Code         = (VOID (*)()) irq_handler;

    printf("my irq a4091_save=%p isr_struct=%p IRQ=%d\n", &a4091_save, &a4091_save.as_isr, A4091_INTPRI);

    AddIntServer(A4091_IRQ, a4091_save.as_isr);
    return (0);
}

static void
a4091_remove_local_irq_handler(void)
{
    if (a4091_save.as_isr != NULL) {
        struct Interrupt *as_isr = a4091_save.as_isr;
        printf("Removing ISR handler (%d irqs)\n", a4091_save.as_irq_count);
        a4091_save.as_exiting = 1;
        a4091_save.as_isr = NULL;
        RemIntServer(A4091_IRQ, as_isr);
        FreeMem(as_isr, sizeof (*a4091_save.as_isr));
#if 0
        Forbid();
        if (a4091_save.as_svc_task != NULL) {
            Signal(a4091_save.as_svc_task, BIT(a4091_save.as_irq_signal));
        }
        Permit();
        while (a4091_save.as_svc_task != NULL) {
            if (count++ > TICKS_PER_SECOND) {
                printf("CMD handler took too long to exit %p\n", &a4091_save.as_svc_task);
                return;  // This is not going to end well...
            }
            Delay(1);
        }
#endif
        FreeSignal(a4091_save.as_irq_signal);
    }
}

/*
 * a4091_find
 * ----------
 * Locates the specified A4091 in the system (by autoconfig order).
 */
static uint32_t
a4091_find(uint32_t pos)
{
    struct Library   *ExpansionBase;
    struct ConfigDev *cdev  = NULL;
    uint32_t          as_addr  = -1;  /* Default to not found */
    int               count = 0;

    if ((ExpansionBase = OpenLibrary(expansion_library_name, 0)) == 0) {
        printf("Could not open %s\n", expansion_library_name);
        return (-1);
    }

    do {
        cdev = FindConfigDev(cdev, ZORRO_MFG_COMMODORE, ZORRO_PROD_A4091);
        if (cdev != NULL) {
            if (pos == count) {
                as_addr = (uint32_t) (cdev->cd_BoardAddr);
                break;
            }
            count++;
        }
    } while (cdev != NULL);

    CloseLibrary(ExpansionBase);

    return (as_addr);
}

int
a4091_validate(uint32_t dev_base)
{
    uint32_t temp1;
    uint32_t temp2;
    uint32_t scratch1;
    uint32_t scratch2;

    siop_regmap_p rp = (siop_regmap_p) (dev_base + 0x00800000);
    if ((dev_base < 0x10000000) || (dev_base >= 0xf0000000)) {
        printf("Invalid device base %x\n", dev_base);
        return (1);
    }

    a4091_reset(dev_base);
    /*
     * Minimally validate device is present by writing the temp and scratch
     * registers.
     */
    scratch1 = rp->siop_scratch;
    temp1    = rp->siop_temp;
    rp->siop_scratch = 0xdeadbeef;
    rp->siop_temp    = 0xaaaa5555;
    scratch2 = rp->siop_scratch;
    temp2    = rp->siop_temp;
    rp->siop_scratch = scratch1;
    rp->siop_temp    = temp1;
    if ((rp->siop_scratch != scratch1) || (rp->siop_temp != temp1) ||
        (scratch2 != 0xdeadbeef) || (temp2 != 0xaaaa5555)) {
        return (1);
    }
    return (0);
}

static int
do_attach(device_t self, uint32_t dev_base, uint scsi_target)
{
    int rc;
    struct siop_softc *sc = device_private(self);
    struct scsipi_adapter *adapt = &sc->sc_adapter;
    struct scsipi_channel *chan = &sc->sc_channel;
    struct scsipi_periph *periph;

    memset(sc, 0, sizeof (*sc));

    printf("do_attach(%x, %x)\n", dev_base, scsi_target);

    if (a4091_validate(dev_base))
        return (1);
    periph = AllocMem(sizeof (*periph), MEMF_CLEAR | MEMF_PUBLIC);
    if (periph == NULL) {
        printf("AllocMem failed\n");
        return (1);
    }
    global_periph = periph;
    periph->periph_channel = chan;  // Not sure this is needed

//  periph->periph_cap |= PERIPH_CAP_SYNC;   // Synchronous SCSI
//  periph->periph_cap |= PERIPH_CAP_TQING;  // Tagged command queuing
    periph->periph_openings = 1;  // Max # of outstanding commands
    periph->periph_target   = (uint8_t)scsi_target;         // SCSI target ID
    periph->periph_lun      = (uint8_t)(scsi_target >> 8);  // SCSI LUN
    periph->periph_dbflags  = SCSIPI_DEBUG_FLAGS;  // Full debugging

    sc->sc_dev = self;

    sc->sc_siopp = (siop_regmap_p)((char *)dev_base + 0x00800000);

    /*
     * CTEST7 = 80 [disable burst]
     */
    sc->sc_clock_freq = 50;     /* Clock = 50 MHz */
    sc->sc_ctest7 = SIOP_CTEST7_CDIS;
    sc->sc_dcntl = SIOP_DCNTL_EA;
    TAILQ_INIT(&sc->ready_list);
    TAILQ_INIT(&sc->nexus_list);
    TAILQ_INIT(&sc->free_list);

    /*
     * Fill in the scsipi_adapter.
     */
    memset(adapt, 0, sizeof (*adapt));
    adapt->adapt_dev = self;
    adapt->adapt_nchannels = 1;
    adapt->adapt_openings = 7;
    adapt->adapt_max_periph = 1;
    adapt->adapt_request = siop_scsipi_request;
    adapt->adapt_minphys = siop_minphys;

    /*
     * Fill in the scsipi_channel.
     */
    memset(chan, 0, sizeof (*chan));
    chan->chan_adapter = adapt;
//  chan->chan_bustype = &scsi_bustype;
    chan->chan_channel = 0;
    chan->chan_ntargets = 8;
    chan->chan_nluns = 8;
    chan->chan_id = 7;
//  chan->scsipi_periph = periph;
#if 0
    TAILQ_INIT(&chan->chan_queue);
    TAILQ_INIT(&chan->chan_complete);
#endif
    scsipi_channel_init(chan);


    rc = a4091_add_local_irq_handler(dev_base);
    if (rc != 0)
        return (rc);

    Signal(a4091_save.as_svc_task, BIT(a4091_save.as_irq_signal));
    siopinitialize(sc);

#if 0
        error = scsipi_test_unit_ready(periph,
            XS_CTL_DISCOVERY | XS_CTL_IGNORE_ILLEGAL_REQUEST |
            XS_CTL_IGNORE_MEDIA_CHANGE | XS_CTL_SILENT_NODEV);
#endif

    /*
     * attach all scsi units on us
     */
//  config_found(self, chan, scsiprint, CFARGS_NONE);
    return (0);
}

int
attach(uint which, uint scsi_target)
{
    uint32_t a4091_base = a4091_find(which);
    if (a4091_base == 0) {
        printf("A4091 not found\n");
        return (1);
    }

    if (do_attach(&device_self, a4091_base, scsi_target))
        return (1);

    printf("attached(%x, %x)\n", a4091_base, scsi_target);
#if 0
    siop_dump(device_private(NULL));  // DEBUG
#endif
    return (0);
}

void
detach(void)
{
    /*
     * XXX: This function needs a lot of work -- it should only detach from
     *      a device which was previously attached, and should not use globals.
     */
    if (global_periph != NULL) {
        uint32_t a4091_base = a4091_find(global_periph->periph_target >> 16);
        if (a4091_base == 0)
            return;
        a4091_reset(a4091_base);
        a4091_remove_local_irq_handler();
        FreeMem(global_periph, sizeof (*global_periph));
    }
}

void cmd_handler(void)
{
    struct IORequest *ior;
    struct IOExtTD   *iotd;
    struct Process   *proc;
    start_msg_t      *msg;
    ULONG             int_mask;
    ULONG             cmd_mask;
    ULONG             wait_mask;
    uint              which_target;
    int               rc;
    uint64_t          blkno;
    uint32_t          mask;
#if 0
    register long devbase asm("a6");
#endif

    proc = (struct Process *)FindTask((char *)NULL);
    printf("cmd_handler()=%p\n", proc);

    /* get the startup message */
    while ((msg = (start_msg_t *) GetMsg(&proc->pr_MsgPort)) == NULL)
        WaitPort(&proc->pr_MsgPort);
//    printf("got startup msg %p\n", msg);

#if 0
    /* Builtin compiler function to set A4 to the global data area */
    geta4();

    devbase = msg->devbase;
    (void) devbase;
#else
    SysBase = *(struct ExecBase **)4UL;
    DOSBase = (struct DosLibrary *) OpenLibrary("dos.library",37L);
#endif
    which_target = msg->which_target;

    myPort = CreatePort(0, 0);
    if (myPort == NULL) {
        /* Terminate handler and give up */
        ReplyMsg((struct Message *)msg);
        Forbid();
        return;
    }
    attach(which_target >> 16, which_target & 0xffff);
    ReplyMsg((struct Message *)msg);

    cmd_mask   = BIT(myPort->mp_SigBit);
    int_mask   = BIT(a4091_save.as_irq_signal);
    wait_mask  = cmd_mask | int_mask;

    while (1) {
//      WaitPort(myPort);
        mask = Wait(wait_mask);

        if (a4091_save.as_exiting)
            break;
        if (mask & int_mask)
            printf("Got INT BH\n");
        irq_poll(mask & int_mask);

        if ((mask & cmd_mask) == 0)
            continue;

        while ((ior = (struct IORequest *)GetMsg(myPort)) != NULL) {
            ior->io_Error = 0;

            switch (ior->io_Command) {
                case CMD_TERM:
                    printf("CMD_TERM\n");
                    detach();
                    printf("Detach done %p\n", &a4091_save.as_isr);
                    CloseLibrary((struct Library *) DOSBase);
                    a4091_save.as_isr = NULL;
                    Forbid();
                    ReplyMsg(&ior->io_Message);
                    return;

                case CMD_READ:
                    iotd = (struct IOExtTD *)ior;
                    iotd->iotd_Req.io_Actual = 1;
                    printf("CMD_READ %lx %lx\n",
                           iotd->iotd_Req.io_Offset, iotd->iotd_Req.io_Length);
                    blkno = iotd->iotd_Req.io_Offset / 512;
                    rc = diskstart(blkno, B_READ, iotd->iotd_Req.io_Data,
                                   iotd->iotd_Req.io_Length, ior);
                    iotd->iotd_Req.io_Error = rc;
                    if (rc == 0) {
                        iotd->iotd_Req.io_Actual = iotd->iotd_Req.io_Length;
                    } else {
                        iotd->iotd_Req.io_Actual = 0;
                        ReplyMsg(&ior->io_Message);
                    }
                    break;

                case CMD_WRITE:
                    iotd = (struct IOExtTD *)ior;
                    iotd->iotd_Req.io_Actual = 1;
                    printf("CMD_WRITE %lx %lx\n",
                           iotd->iotd_Req.io_Offset, iotd->iotd_Req.io_Length);
#if 0
ior->io_Error = IOERR_NOCMD;
ReplyMsg(&ior->io_Message);
continue;
#endif
                    blkno = iotd->iotd_Req.io_Offset / 512;
                    rc = diskstart(blkno, B_READ, iotd->iotd_Req.io_Data,
                                   iotd->iotd_Req.io_Length, ior);
                    iotd->iotd_Req.io_Error = rc;
                    if (rc == 0) {
                        iotd->iotd_Req.io_Actual = iotd->iotd_Req.io_Length;
                    } else {
                        iotd->iotd_Req.io_Actual = 0;
                        ReplyMsg(&ior->io_Message);
                    }
                    break;

                default:
                    /* Unknown command */
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
create_cmd_handler(uint scsi_target)
{
    struct Process *myProc;
    start_msg_t msg;
    register long devbase asm("a6");

    DOSBase = (struct DosLibrary *) OpenLibrary("dos.library",37L);
    if (DOSBase == NULL)
        return (1);

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
    msg.which_target = scsi_target;
    PutMsg(&myProc->pr_MsgPort, (struct Message *)&msg);
    WaitPort(msg.msg.mn_ReplyPort);
    DeletePort(msg.msg.mn_ReplyPort);

    if (myPort == NULL) /* CMD_Handler allocates this */
        return (1);

    return (0);
}

