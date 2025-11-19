//
// Copyright 2022-2025 Stefan Reinauer & Chris Hooper
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//

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
#include "battmem.h"
#include "ndkcompat.h"

#include "a4091.h"
#include "util/a4092flash/flash.h"
#include "util/a4092flash/nvram_flash.h"

/*
 * NewMinList
 * ----------
 * Exec V45+ contains the NewMinList function
 * But this is not available in Kickstart 3.1
 * So we undefine the NDK function and implement it ourselves.
 */
#undef NewMinList
void NewMinList(struct MinList * list)
{
    list->mlh_Tail     = NULL;
    list->mlh_Head     = (struct MinNode *)&list->mlh_Tail;
    list->mlh_TailPred = (struct MinNode *)list;
}


void scsipi_free_all_xs(struct scsipi_channel *chan);

extern struct ExecBase *SysBase;
extern int romboot;
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
static int
irq_handler_core(a4091_save_t *save)
{
    struct siop_softc *sc = save->as_device_private;
    siop_regmap_p      rp;
    uint8_t            istat;
    uint32_t           reg;

    if (sc->sc_flags & SIOP_INTSOFF)
        return (0); /* interrupts are not active */

    rp = sc->sc_siopp;
    istat = rp->siop_istat;
    if ((istat & (SIOP_ISTAT_SIP | SIOP_ISTAT_DIP)) == 0)
        return (0);
    save->as_irq_count++;

    /*
     * Save Interrupt Status, SCSI Status 0, and DMA Status.
     *
     * SCSI Status 0 and DMA Status are read in the same transaction
     * to ensure that interrupts clear properly and that SCSI interrupts
     * captured in ISTAT are not missed. See DMA Status (DSTAT) register
     * documentation.
     */
    sc->sc_istat |= istat;
    reg = *ADDR32((uintptr_t) &rp->siop_sstat2);
    sc->sc_sstat0 = reg >> 8;
    sc->sc_dstat  = reg;

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
static LONG __saveds
irq_handler(register a4091_save_t *save asm("a1"))
{
    return (irq_handler_core(save));
}

void *
device_private(device_t dev)
{
    (void)dev;
    /* CDH: HACK - This should be per device but it is global */
    return (asave->as_device_private);
}

#ifdef ENABLE_QUICKINTS

#include "quickints.c"

/*
 * a4091_quick_irq
 * -----------------------
 * Assembly wrapper for quick interrupts that calls the existing
 * irq_handler() C function. This reuses all the existing interrupt
 * handling logic.
 */
void __stdargs a4091_quick_irq(void);
asm (
    "    .globl _a4091_quick_irq         \n"
    "_a4091_quick_irq:                   \n"
    "    move.l  d0,-(sp)                \n"
    "    move.w  0xdff01c,d0             \n" // Interrupt Enable State
    "    btst.l  #14,d0                  \n" // Check if pending disable
    "    bne.s   RealInterrupt           \n"
    "ExitInt:                            \n"
    "    move.l  (sp)+,d0                \n"
    "    rte                             \n"
    "RealInterrupt:                      \n"
    "    movem.l d1/a0-a1,-(sp)          \n" // Save registers for C ABI
    "    move.l  _asave,a1               \n" // Load pointer to a4091_save
    "    jsr     _irq_handler            \n" // Call C handler (a1 passed in register)
    "    movem.l (sp)+,d1/a0-a1          \n" // Restore registers
    "    bra.s   ExitInt                 \n" // Return from interrupt
);

/*
 * a4091_add_quick_irq_handler
 * ---------------------------
 * Set up quick interrupt handling for the A4091 card.
 */
static int
a4091_add_quick_irq_handler(uint32_t a4091_base)
{
    ULONG intnum;
    struct Task *task = FindTask(NULL);
    if (task == NULL)
        return (ERROR_OPEN_FAIL);

    asave->as_SysBase    = SysBase;
    asave->as_svc_task   = task;
    asave->as_irq_count  = 0;
    asave->as_irq_signal = AllocSignal(-1);

    // Obtain a quick interrupt vector
    intnum = ObtainQuickVector(a4091_quick_irq);
    if (intnum == 0) {
        printf("Failed to obtain quick interrupt vector\n");
        FreeSignal(asave->as_irq_signal);
        return (ERROR_NO_FREE_STORE);
    }

    asave->quick_vec_num = intnum;

    // Program the A4091 to use this quick interrupt vector
    *ADDR8(a4091_base + A4091_OFFSET_QUICKINT) = (uint8_t)intnum;

    printf("Quick interrupt vector %lu installed at A4091\n", intnum);
    return (0);
}

/*
 * a4091_remove_quick_irq_handler
 * ------------------------------
 * Remove quick interrupt handling and restore the vector.
 */
static void
a4091_remove_quick_irq_handler(void)
{
    if (asave->quick_vec_num != 0) {
        printf("Removing quick ISR handler (%d irqs)\n", asave->as_irq_count);
        asave->as_exiting = 1;
        ReleaseQuickVector(asave->quick_vec_num);

        /* Reset quick interrupt in hardware by writing to upper half of INTVEC range */
        *ADDR8(asave->as_addr + (A4091_OFFSET_QUICKINT | (1<<17))) = 0x26;

        asave->quick_vec_num = 0;
        FreeSignal(asave->as_irq_signal);
    }
}
#endif

static int
a4091_add_local_irq_handler(void)
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
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
    asave->as_isr->is_Code         = (void (*)()) irq_handler;
#pragma GCC diagnostic pop

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

#ifdef DRIVER_A4091
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

#if DRIVER_A4092
    UBYTE manufId,devId;
    ULONG sectorSize;
    ULONG flashSize;
    struct nvram_t nvram;

    if (flash_init(&manufId,&devId,(void *)as_addr,&flashSize,&sectorSize)) {
        if(!flash_read_nvram(NVRAM_OFFSET, &nvram)) {
            *(volatile uint8_t *)(as_addr + HW_OFFSET_SWITCHES) =
		    nvram.settings.switch_flags;
	}
    }
#endif

    CloseLibrary((struct Library *)ExpansionBase);

    asave->as_addr = as_addr;
    asave->as_cd = cdev;

    return (as_addr);
}
#else
/*
 * a4000t_find
 * ----------
 * Locates the next A4091 in the system (by autoconfig order) which has
 * not yet been claimed by a driver. If one is not found, the first
 * device is chosen (reused).
 */
static uint32_t
a4000t_find(UBYTE *boardnum)
{
    *boardnum=0;
    asave->as_addr = HW_SCSI_BASE;
    asave->as_cd = NULL;
    return asave->as_addr;
}
#endif

uint8_t get_dip_switches(void)
{
    uint8_t switches;
#if defined(DRIVER_A4000T) || defined(DRIVER_A4000T770)
    switches = *(volatile uint32_t *)(asave->as_addr + HW_OFFSET_SWITCHES);
#else
    switches = *(volatile uint8_t *)(asave->as_addr + HW_OFFSET_SWITCHES);
#endif
    return switches;
}

static void
a4091_release(uint32_t as_addr)
{
    if (asave->as_addr != as_addr)
        printf("Releasing wrong card.\n");
#if HW_IS_ZORRO3
    asave->as_cd->cd_Flags |= CDB_CONFIGME;
#endif
}

static int
a4091_validate(uint32_t dev_base)
{
    uint32_t temp;
    uint32_t scratch;
    uint32_t patt = 0xf0e7c3a5;
    uint32_t next;
    uint     rot;
    uint     fail = 0;

    siop_regmap_p rp = (siop_regmap_p) (dev_base + HW_OFFSET_REGISTERS);
#if HW_IS_ZORRO3
    if ((dev_base < 0x10000000) || (dev_base >= 0xf0000000) ||
        (dev_base & 0x00ffffff)) {
        printf("Invalid device base %x\n", dev_base);
        return (ERROR_BAD_BOARD);
    }
#endif
#if !HW_IS_ZORRO3
        rp->siop_dcntl = SIOP_DCNTL_EA;
#endif
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
#if defined(NCR53C770)
#define siop_scratch siop_scratcha
#endif
    /* Create write pointer offset for 68030 cache write-allocate workaround */
#if defined(DRIVER_A4091)
    siop_regmap_p rp_write = (siop_regmap_p)((char *)rp + 0x40);
#elif defined(DRIVER_A4000T)
    siop_regmap_p rp_write = (siop_regmap_p)((char *)rp + 0x80);
#elif defined(DRIVER_A4000T770)
    siop_regmap_p rp_write = (siop_regmap_p)((char *)rp + 0x200);
#endif

    scratch = rp->siop_scratch;
    temp    = rp->siop_temp;
    for (rot = 0; rot < 32; rot++, patt = next) {
        uint32_t got_scratch;
        uint32_t got_temp;
        next = ((patt & 0x7fffffff) << 1) | (patt >> 31);
        rp_write->siop_scratch = patt;
        rp_write->siop_temp = next;

        /*
         * The cache line flushes below serve two purposes:
         * 1) 68040 cache might still be on for the Zorro region during
         *    pre-boot init. This can cause the wrong values to be read
         *    from the scratch and test registers the writes to the
         *    shadow registers do not update the cached values of the
         *    primary registers.
         * 2) 68030 cache write-allocate mode bug work-around. This is
         *    where even if the cache is disabled for a region, the CPU
         *    in rare cases might allocate a cache line on write.
         *    This is actually not needed for 68030 because this code
         *    does not write to the same register address as what it
         *    reads (shadow registers of the 53C710 in the A4091 are
         *    used for the write accesses).
         */
        CacheClearE((void *)(&rp->siop_scratch), 4, CACRF_ClearD);
        CacheClearE((void *)(&rp->siop_temp), 4, CACRF_ClearD);

        got_scratch = rp->siop_scratch;
        got_temp = rp->siop_temp;
        if ((got_scratch != patt) ||
            (got_temp != next)) {
            printf(XSTR(DEVNAME)" FAIL");
            if (got_scratch != patt) {
                printf(" scratch %08x != %08x [%08x]",
                       got_scratch, patt, got_scratch ^ patt);
                panic("Hardware test failure\n  SCRATCH %08lx != %08lx [%08lx]",
                       got_scratch, patt, got_scratch ^ patt);
            }
            if (got_temp != next) {
                printf(" temp %08x != %08x [%08x]",
                       got_temp, next, got_temp ^ next);
                panic("Hardware test failure\n  TEMP %08lx != %08lx [%08lx]",
                       got_temp, next, got_temp ^ next);
            }
            printf("\n");
            fail++;
            break;
        }
    }
    rp_write->siop_scratch = scratch;
    rp_write->siop_temp    = temp;

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

#ifdef DRIVER_A4091
    dev_base = a4091_find(boardnum);
#else
    dev_base = a4000t_find(boardnum);
#endif
    if (dev_base == 0) {
        printf(XSTR(DEVNAME)": board #%u not found\n",*boardnum);
        return (ERROR_NO_BOARD);
    }

    printf(XSTR(DEVNAME)": board #%u found at 0x%x\n", *boardnum, dev_base);
    if ((rc = a4091_validate(dev_base)))
        return (rc);

    memset(sc, 0, sizeof (*sc));
    dip_switches = get_dip_switches();
    printf("DIP switches = %02x\n", dip_switches);

    Load_BattMem();

    sc->sc_dev = self;
    sc->sc_siopp = (siop_regmap_p)((char *)dev_base + HW_OFFSET_REGISTERS);
    sc->sc_clock_freq = HW_CLOCK_FREQ;     /* SCSI Host Controller Clock */
#ifdef NCR53C710
    sc->sc_ctest7 = SIOP_CTEST7_CDIS;  // Disable burst
#elif NCR53C770
    sc->sc_ctest0 = SIOP_CTEST0_CDIS;  // Disable burst
#else
#error "Unsupported SCSI Host Controller"
#endif
#if defined(DRIVER_A4000T) || defined(DRIVER_A4000T770)
    sc->sc_dcntl = SIOP_DCNTL_EA;  /* A4000T: Connect _SLACK/_STERM */
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
#ifdef NCR53C710
    adapt->adapt_request = siop_scsipi_request;
#elif NCR53C770
    adapt->adapt_request = siopng_scsipi_request;
#else
#error ""
#endif
    adapt->adapt_asave = asave;

    /*
     * Fill in the scsipi_channel.
     */
    memset(chan, 0, sizeof (*chan));
    chan->chan_adapter = adapt;
    chan->chan_nluns = (dip_switches & BIT(7)) ? 1 : 8;  // SCSI LUNs enabled?
    chan->chan_id = dip_switches & 7;  // SCSI ID from DIP switches
    TAILQ_INIT(&chan->chan_queue);
    TAILQ_INIT(&chan->chan_complete);

    asave->as_callout_head = &callout_head;

    if ((dip_switches & BIT(5)) == 0) {
        /* Need to disable synchronous SCSI */
        sc->sc_nosync = ~0;
    }
    sc->sc_nodisconnect = 0;  /* Mask of targets not allowed to disconnect */

    /*
     * A4091 Rear-access DIP switches
     *   SW 8 Off  SCSI LUNs Enabled                chan->chan_nluns
     *   SW 7 Off  Internal Termination On          Handled by hardware
     *   SW 6 Off  Synchronous SCSI Mode            sc->sc_nosync
     *   SW 5 Off  Short Spinup                     ms.slowSpinup
     *   SW 4 Off  SCSI-2 Fast Bus Mode             NOT SUPPORTED YET
     *   SW 3 Off  ADR2=1                           chan->chan_id
     *   SW 2 Off  ADR1=1                           chan->chan_id
     *   SW 1 Off  ADR0=1  Controller Host ID=7     chan->chan_id
     */

    scsipi_channel_init(chan);

#ifdef ENABLE_QUICKINTS
    // Use quick interrupts if enabled in battmem, otherwise use normal interrupts
    if (asave->quick_int)
        rc = a4091_add_quick_irq_handler(dev_base);
    else
#endif
        rc = a4091_add_local_irq_handler();

    if (rc != 0)
        return (rc);

    Signal(asave->as_svc_task, BIT(asave->as_irq_signal));
#ifdef NCR53C710
    siopinitialize(sc);
#elif NCR53C770
    siopnginitialize(sc);
#else
#error "Need to define NCR53C710 or NCR53C770"
#endif
    return (0);
}

void
deinit_chan(device_t self)
{
    struct siop_softc     *sc = device_private(self);
    struct scsipi_channel *chan = &sc->sc_channel;

#ifdef NCR53C710
    siopshutdown(chan);
#elif NCR53C770
    siopngshutdown(chan);
#else
#error "Need to define NCR53C710 or NCR53C770"
#endif
#ifdef ENABLE_QUICKINTS
    // Remove quick or normal interrupt based on what was installed
    if (asave->quick_int && asave->quick_vec_num != 0)
        a4091_remove_quick_irq_handler();
    else
#endif
        a4091_remove_local_irq_handler();

    a4091_release((uint32_t) sc->sc_siopp - 0x00800000);
}

struct scsipi_periph *
scsipi_alloc_periph(int flags)
{
    struct scsipi_periph *periph;
    uint i;
    (void)flags;

    periph = AllocMem(sizeof (*periph), MEMF_PUBLIC | MEMF_CLEAR);
    if (periph == NULL)
        return (NULL);

#if 0
    /*
     * Start with one command opening.  The periph driver
     * will grow this if it knows it can take advantage of it.
     */
    periph->periph_openings = 1;
#endif

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
attach(device_t self, uint scsi_target, struct scsipi_periph **periph_p,
       uint flags)
{
    struct siop_softc     *sc = device_private(self);
    struct scsipi_channel *chan = &sc->sc_channel;
    struct scsipi_periph  *periph;
    int target, lun;
    int rc;
    int failed = 0;
    (void)flags;

    decode_unit_number(scsi_target, &target, &lun);

    // Check for NCR53c770 wide SCSI limits: 16 targets (0-15), 8 LUNs (0-7)
    if (target >= 16 || lun >= 8)
        return (ERROR_OPEN_FAIL);

    if (target == chan->chan_id)
        return (ERROR_SELF_UNIT);

    periph = scsipi_alloc_periph(0);
    *periph_p = periph;
    if (periph == NULL)
        return (ERROR_NO_MEMORY);
    printf("attach(%p, %d)\n", periph, scsi_target);
    periph->periph_openings  = 4;  // Max # of outstanding commands
    periph->periph_target    = target;              // SCSI target ID
    periph->periph_lun       = lun;                 // SCSI LUN
#ifdef DEBUG_SCSIPI
    periph->periph_dbflags   = SCSIPI_DEBUG_FLAGS;  // Full debugging
#else
    periph->periph_dbflags   = 0;
#endif
    periph->periph_changenum = 1;
    periph->periph_channel   = chan;
    periph->periph_changeint = NULL;
    NewMinList(&periph->periph_changeintlist);

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
#if 0
    /* Might be needed for A3000 / A2091 / A590 */
    scsipi_set_xfer_mode(chan, target, 1);
#endif

    return (0);
}

ULONG
calculate_unit_number(int target, int lun)
{
    if (target > 7 || lun > 7) {
        // Phase V wide SCSI scheme for IDs/LUNs > 7
        return lun * 10 * 1000 + target * 10 + HD_WIDESCSI;
    } else {
        // Traditional scheme for IDs/LUNs <= 7
        return target + lun * 10;
    }
}

void
decode_unit_number(ULONG unit_num, int *target, int *lun)
{
    if ((unit_num % 10) == HD_WIDESCSI) {
        // Phase V wide SCSI scheme
        *target = (unit_num / 10) % 1000;
        *lun = (unit_num / (10 * 1000)) % 1000;
    } else {
        // Traditional scheme
        *target = unit_num % 10;
        *lun = unit_num / 10;
    }
}

void
detach(struct scsipi_periph *periph)
{
    printf("detach(%p, %d)\n",
           periph, calculate_unit_number(periph->periph_target, periph->periph_lun));

    if (periph != NULL) {
        int timeout = 6;  // Seconds
        struct scsipi_channel *chan = periph->periph_channel;
        while (periph->periph_sent > 0) {
            /* Need to wait for outstanding commands to complete */
            timeout -= irq_and_timer_handler();
            if (timeout == 0) {
                printf("Detach timeout waiting for periph to quiesce\n");
                return;
            }
        }
        scsipi_remove_periph(chan, periph);
        scsipi_free_periph(periph);
    }
}

int
periph_still_attached(void)
{
    uint                   i;
    struct siop_softc     *sc = asave->as_device_private;
    struct scsipi_channel *chan = &sc->sc_channel;

    for (i = 0; i < SCSIPI_CHAN_PERIPH_BUCKETS; i++)
        if (LIST_FIRST(&chan->chan_periphtab[i]) != NULL) {
            return (1);
        }
    return (0);
}
