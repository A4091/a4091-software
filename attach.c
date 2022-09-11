#ifdef DEBUG_ATTACH
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
#include <proto/expansion.h>
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
#include "ndkcompat.h"

void scsipi_free_all_xs(struct scsipi_channel *chan);

#define ZORRO_MFG_COMMODORE     0x0202
#define ZORRO_PROD_A4091        0x0054

#define A4091_OFFSET_REGISTERS  0x00800000

#define A4091_INTPRI 30
#define A4091_IRQ    3

#define ADDR8(x)      (volatile uint8_t *)(x)
#define ADDR32(x)     (volatile uint32_t *)(x)

#define BIT(x)        (1 << (x))

extern struct ExecBase *SysBase;
extern a4091_save_t *asave;
extern int romboot;
extern char real_device_name[];
struct ExpansionBase *ExpansionBase;

/*
 * irq_handler_core
 * ----------------
 * This is the actual interrupt handler. It checks whether the interrupt
 * register of the SCSI chip indicates there is something to do, and if
 * so also captures the SCSI Status 0 and DMA Status, and then wakes the
 * service task to go process them.
 */
__attribute__((noinline))
int
irq_handler_core(a4091_save_t *save)
{
    struct siop_softc *sc = &save->as_device_private;
    siop_regmap_p      rp;
    uint8_t            istat;

    if (sc->sc_flags & SIOP_INTSOFF)
        return (0); /* interrupts are not active */

    rp = sc->sc_siopp;
    istat = rp->siop_istat;
    if ((istat & (SIOP_ISTAT_SIP | SIOP_ISTAT_DIP)) == 0)
        return (0);
    save->as_irq_count++;

    /*
     * save Interrupt Status, SCSI Status 0, and DMA Status.
     */
    sc->sc_istat |= istat;
    sc->sc_sstat0 = rp->siop_sstat0;
    sc->sc_dstat  = rp->siop_dstat;

    if (save->as_svc_task != NULL)
        Signal(save->as_svc_task, BIT(save->as_irq_signal));

    return (!!(save->as_irq_count & 0xf));
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
    return (irq_handler_core(save));
}

/* CDH: HACK */
void *
device_private(device_t dev)
{
    return (&asave->as_device_private);
}
/* CDH: HACK */

static int
a4091_add_local_irq_handler(uint32_t dev_base)
{
    struct Task *task = FindTask(NULL);
    if (task == NULL)
        return (ERROR_OPEN_FAIL);

    asave->as_SysBase        = SysBase;
    asave->as_svc_task       = task;
    asave->as_irq_count      = 0;
    asave->as_irq_signal     = AllocSignal(-1);
    asave->as_isr            = AllocMem(sizeof (*asave->as_isr),
                                        MEMF_CLEAR | MEMF_PUBLIC);
    if (asave->as_isr == NULL) {
        printf("AllocMem failed\n");
        return (ERROR_NO_MEMORY);
    }

    asave->as_isr->is_Node.ln_Type = NT_INTERRUPT;
    asave->as_isr->is_Node.ln_Pri  = A4091_INTPRI;
    asave->as_isr->is_Node.ln_Name = real_device_name; // a4091.device
    asave->as_isr->is_Data         = asave;
    asave->as_isr->is_Code         = (void (*)()) irq_handler;

    printf("Add IRQ=%d pri=%d isr=%p asave=%p\n",
           A4091_IRQ, A4091_INTPRI, &asave->as_isr, asave);

    AddIntServer(A4091_IRQ, asave->as_isr);
    return (0);
}

static void
a4091_remove_local_irq_handler(void)
{
    if (asave->as_isr != NULL) {
        struct Interrupt *as_isr = asave->as_isr;
        printf("Removing ISR handler (%d irqs)\n", asave->as_irq_count);
        asave->as_exiting = 1;
        asave->as_isr = NULL;
        RemIntServer(A4091_IRQ, as_isr);
        FreeMem(as_isr, sizeof (*asave->as_isr));
        FreeSignal(asave->as_irq_signal);
    }
}

/*
 * a4091_find
 * ----------
 * Locates the next A4091 in the system (by autoconfig order) which has
 * not yet been claimed by a driver. If one is not found, the first
 * device is chosen (reused).
 */
static uint32_t
a4091_find(UBYTE *boardnum)
{
    struct ConfigDev *cdev  = NULL;
    uint32_t          as_addr  = 0;  /* Default to not found */
    int               count = 0;

    if ((ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 0)) == 0) {
        printf("Can't open expansion.library.\n");
        return (0);
    }

    if (romboot){
        /*
         * ROM code needs to be using GetCurrentBinding() rather
         * than FindConfigDev() to get the current board.
         */
        ULONG res;
        struct CurrentBinding cb;
        res = GetCurrentBinding(&cb, sizeof (cb));
        printf("gcb=%"PRIu32" fn='%s' ps='%s'\n", res,
		(char *)cb.cb_FileName ?: "", (char *)cb.cb_ProductString ?: "");
        if (!res)
            return (0);
        if (cb.cb_ConfigDev != NULL) {
            struct ConfigDev *cd = cb.cb_ConfigDev;
	    cdev = cd;
            as_addr = (uint32_t) (cdev->cd_BoardAddr);
            do {
                printf("configdev %p board=%08x flags=%02x configme=%x driver=%p\n",
			cd, (uint32_t) cd->cd_BoardAddr, cd->cd_Flags, CDB_CONFIGME, cd->cd_Driver);
                cd = cd->cd_NextCD;
            } while (cd != NULL);
        }
    } else {
        do {
            cdev = FindConfigDev(cdev, ZORRO_MFG_COMMODORE, ZORRO_PROD_A4091);
            if ((cdev != NULL) && (cdev->cd_Flags & CDB_CONFIGME)) {
                cdev->cd_Flags &= ~CDB_CONFIGME;
                as_addr = (uint32_t) (cdev->cd_BoardAddr);
                *boardnum = count;
                break;
            }
            count++;
        } while (cdev != NULL);

        if (cdev == NULL) {
            cdev = FindConfigDev(cdev, ZORRO_MFG_COMMODORE, ZORRO_PROD_A4091);
            if (cdev != NULL) {
                /* Just take the first board found */
                as_addr = (uint32_t) (cdev->cd_BoardAddr);
                *boardnum = 0;
            }
        }
    }

    CloseLibrary((struct Library *)ExpansionBase);

    asave->as_addr = as_addr;
    asave->as_cd = cdev;

    return (as_addr);
}

static void
a4091_release(uint32_t as_addr)
{
    if (asave->as_addr != as_addr)
        printf("Releasing wrong card.\n");
    asave->as_cd->cd_Flags |= CDB_CONFIGME;
}

int
a4091_validate(uint32_t dev_base)
{
    uint32_t temp;
    uint32_t scratch;
    uint32_t patt = 0xf0e7c3a5;
    uint32_t next;
    uint     rot;
    uint     fail = 0;

    siop_regmap_p rp = (siop_regmap_p) (dev_base + 0x00800000);
    if ((dev_base < 0x10000000) || (dev_base >= 0xf0000000) ||
        (dev_base & 0x00ffffff)) {
        printf("Invalid device base %x\n", dev_base);
        return (ERROR_BAD_BOARD);
    }

#if 0
    rp->siop_istat |= SIOP_ISTAT_RST;       /* reset chip */
    delay(20000);
    rp->siop_istat &= ~SIOP_ISTAT_RST;
    delay(20000);
#endif

    /*
     * Validate device connectivity by writing the temp and scratch
     * registers.
     */
    scratch = rp->siop_scratch;
    temp    = rp->siop_temp;
    for (rot = 0; rot < 32; rot++, patt = next) {
        uint32_t got_scratch;
        uint32_t got_temp;
        next = (patt << 1) | (patt >> 31);
        rp->siop_scratch2 = patt;
        rp->siop_temp2 = next;

#if 0
        /* 68030 cache write-allocate mode bug work-around */
        CacheClearE((void *)(&rp->siop_scratch), 4, CACRF_ClearD);
        CacheClearE((void *)(&rp->siop_temp), 4, CACRF_ClearD);
#endif

        got_scratch = rp->siop_scratch;
        got_temp = rp->siop_temp;
        if ((got_scratch != patt) ||
            (got_temp != next)) {
            printf("A4091 FAIL");
            if (got_scratch != patt) {
                printf(" scratch %08x != %08x [%08x]",
                       got_scratch, patt, got_scratch ^ patt);
            }
            if (got_temp != next) {
                printf(" temp %08x != %08x [%08x]",
                       got_temp, next, got_temp ^ next);
            }
            printf("\n");
            fail++;
            break;
        }
    }
    rp->siop_scratch2 = scratch;
    rp->siop_temp2    = temp;

    return (fail ? ERROR_BAD_BOARD : 0);
}

int
init_chan(device_t self, UBYTE *boardnum)
{
    struct siop_softc     *sc = device_private(self);
    struct scsipi_adapter *adapt = &sc->sc_adapter;
    struct scsipi_channel *chan = &sc->sc_channel;
    uint32_t dev_base;
    uint8_t dip_switches;
    int rc;

    dev_base = a4091_find(boardnum);
    if (dev_base == 0) {
        printf("A4091: board #%u not found\n",*boardnum);
        return (ERROR_NO_BOARD);
    }

    printf("A4091: board #%u found at 0x%x\n", *boardnum, dev_base);
    if ((rc = a4091_validate(dev_base)))
        return (rc);

    CopyMem("a4091", self->dv_xname, 6);

    memset(sc, 0, sizeof (*sc));
    dip_switches = *(uint8_t *)(dev_base + 0x008c0003);
    printf("DIP switches = %02x\n", dip_switches);
    sc->sc_dev = self;
    sc->sc_siopp = (siop_regmap_p)((char *)dev_base + 0x00800000);
    sc->sc_clock_freq = 50;     /* Clock = 50 MHz */
    sc->sc_ctest7 = SIOP_CTEST7_CDIS;  // Disable burst
#ifdef DRIVER_A4000T
    sc->sc_dcntl = SIOP_DCNTL_EA;  /* A4000T */
#else
    sc->sc_dcntl = 0;              /* A4091 */
#endif
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
    adapt->adapt_asave = asave;

    /*
     * Fill in the scsipi_channel.
     */
    memset(chan, 0, sizeof (*chan));
    chan->chan_adapter = adapt;
    chan->chan_nluns = (dip_switches & BIT(7)) ? 8 : 1;  // SCSI LUNs enabled?
    chan->chan_id = dip_switches & 7;  // SCSI ID from DIP switches
    TAILQ_INIT(&chan->chan_queue);
    TAILQ_INIT(&chan->chan_complete);

    asave->as_callout_head = &callout_head;

    if ((dip_switches & BIT(5)) == 0) {
        /* Need to disable synchronous SCSI */
        sc->sc_nosync = ~0;
    }

    /*
     * A4091 Rear-access DIP switches
     *   SW 8 Off  SCSI LUNs Enabled                chan->chan_nluns
     *   SW 7 Off  Internal Termination On          Handled by hardware
     *   SW 6 Off  Synchronous SCSI Mode            sc->sc_nosync
     *   SW 5 Off  Short Spinup                     NOT SUPPORTED YET
     *   SW 4 Off  SCSI-2 Fast Bus Mode             NOT SUPPORTED YET
     *   SW 3 Off  ADR2=1                           chan->chan_id
     *   SW 2 Off  ADR1=1                           chan->chan_id
     *   SW 1 Off  ADR0=1  Controller Host ID=7     chan->chan_id
     */

    scsipi_channel_init(chan);

    rc = a4091_add_local_irq_handler(dev_base);
    if (rc != 0)
        return (rc);

    Signal(asave->as_svc_task, BIT(asave->as_irq_signal));
    siopinitialize(sc);
    return (0);
}

void
deinit_chan(device_t self)
{
    struct siop_softc     *sc = device_private(self);
    struct scsipi_channel *chan = &sc->sc_channel;

    siopshutdown(chan);
    a4091_remove_local_irq_handler();
    a4091_release((uint32_t) sc->sc_siopp - 0x00800000);
}

struct scsipi_periph *
scsipi_alloc_periph(int flags)
{
    struct scsipi_periph *periph;
    uint i;

    periph = AllocMem(sizeof (*periph), MEMF_PUBLIC | MEMF_CLEAR);
    if (periph == NULL)
        return (NULL);

    /*
     * Start with one command opening.  The periph driver
     * will grow this if it knows it can take advantage of it.
     */
    periph->periph_openings = 1;

    for (i = 0; i < PERIPH_NTAGWORDS; i++)
            periph->periph_freetags[i] = 0xffffffff;

#if 0
    /* Not tracked for AmigaOS */
    TAILQ_INIT(&periph->periph_xferq);
#endif
    return (periph);
}

void
scsipi_free_periph(struct scsipi_periph *periph)
{
    FreeMem(periph, sizeof (*periph));
}

int scsi_probe_device(struct scsipi_channel *chan, int target, int lun, struct scsipi_periph *periph, int *failed);

int
attach(device_t self, uint scsi_target, struct scsipi_periph **periph_p)
{
    struct siop_softc     *sc = device_private(self);
    struct scsipi_channel *chan = &sc->sc_channel;
    struct scsipi_periph  *periph;
    int target = scsi_target % 10;
    int lun    = (scsi_target / 10) % 10;
    int rc;
    int failed = 0;

    if ((scsi_target % 10) == chan->chan_id)
        return (ERROR_SELF_UNIT);

    periph = scsipi_alloc_periph(0);
    *periph_p = periph;
    if (periph == NULL)
        return (ERROR_NO_MEMORY);
    printf("attach(%p, %d)\n", periph, scsi_target);
    periph->periph_openings  = 4;  // Max # of outstanding commands
    periph->periph_target    = scsi_target % 10;         // SCSI target ID
    periph->periph_lun       = (scsi_target / 10) % 10;  // SCSI LUN
    periph->periph_dbflags   = SCSIPI_DEBUG_FLAGS;       // Full debugging
    periph->periph_dbflags   = 0;
    periph->periph_changenum = 1;
    periph->periph_channel   = chan;

    rc = scsi_probe_device(chan, target, lun, periph, &failed);
    printf("scsi_probe_device(%d.%d) cont=%d failed=%d\n",
           target, lun, rc, failed);
#ifndef USE_SERIAL_OUTPUT
    (void) rc;
#endif

    if (failed) {
        scsipi_free_periph(periph);
        return (failed);
    }

    scsipi_insert_periph(chan, periph);

    return (0);
}

void
detach(struct scsipi_periph *periph)
{
    printf("detach(%p)\n", periph);

    if (periph != NULL) {
        struct scsipi_channel *chan = periph->periph_channel;
        scsipi_remove_periph(chan, periph);
        scsipi_free_periph(periph);
    }
}
