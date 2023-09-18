/*
 * A4091 Test Utility
 * ------------------
 * Utility to inspect and test an installed A4091 SCSI controller card
 * for correct operation.
 *
 * Copyright 2023 Chris Hooper.  This program and source may be used
 * and distributed freely, for any purpose which benefits the Amiga
 * community. Commercial use of the binary, source, or algorithms requires
 * prior written or email approval from Chris Hooper <amiga@cdh.eebugs.com>.
 * All redistributions must retain this Copyright notice.
 *
 * DISCLAIMER: THE SOFTWARE IS PROVIDED "AS-IS", WITHOUT ANY WARRANTY.
 * THE AUTHOR ASSUMES NO LIABILITY FOR ANY DAMAGE ARISING OUT OF THE USE
 * OR MISUSE OF THIS UTILITY OR INFORMATION REPORTED BY THIS UTILITY.
 */
const char *version = "\0$VER: A4091 1.0 ("__DATE__") © Chris Hooper";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libraries/expansionbase.h>
#include <clib/expansion_protos.h>
#include <inline/exec.h>
#include <inline/expansion.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <exec/execbase.h>
#include <exec/lists.h>
#include <inline/alib.h>
#include <sys/time.h>
#include "ndkcompat.h"
#include "a4091.h"

/*
 * gcc clib2 headers are bad (for example, no stdint definitions) and are
 * not being included by our build.  Because of that, we need to fix up
 * some stdio definitions.
 */
extern struct iob ** __iob;
#undef stdout
#define stdout ((FILE *)__iob[1])

#define CACHE_LINE_WRITE(addr, len) CacheClearE((void *)(addr), len, \
                                                CACRF_ClearD)
#define CACHE_LINE_DISCARD(addr, len) CacheClearE((void *)(addr), len, \
                                                  CACRF_InvalidateD)

extern struct ExecBase *SysBase;

#define ADDR8(x)      (volatile uint8_t *)(x)
#define ADDR32(x)     (volatile uint32_t *)(x)

#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))
#define BIT(x)        (1 << (x))

#define FLAG_DEBUG            0x01        /* Debug output */
#define FLAG_MORE_DEBUG       0x02        /* More debug output */
#define FLAG_IS_A4000T        0x04        /* A4000T onboard SCSI controller */

#define SUPERVISOR_STATE_ENTER()    { \
                                      APTR old_stack = SuperState()
#define SUPERVISOR_STATE_EXIT()       UserState(old_stack); \
                                    }
#define INTERRUPTS_DISABLE() Disable()  /* Disable Interrupts */
#define INTERRUPTS_ENABLE()  Enable()   /* Enable Interrupts */

#define FIRST(n)   (struct Interrupt *)((n)->lh_Head)
#define EMPTY(n)   ((n)->lh_Head == (struct Node *)&n->lh_Tail)
#define NEXTINT(i) (struct Interrupt *)((i)->is_Node.ln_Succ)
#define LAST(i)    (((i)->is_Node.ln_Succ)->ln_Succ == 0)


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

#define REG_SCNTL0_EPG  BIT(2)  // Generate parity on the SCSI bus

#define REG_SIEN_PAR    BIT(0)  // Interrupt on parity error
#define REG_SIEN_RST    BIT(1)  // Interrupt on SCSI reset received
#define REG_SIEN_UDC    BIT(2)  // Interrupt on Unexpected disconnect
#define REG_SIEN_SGE    BIT(3)  // Interrupt on SCSI gross error
#define REG_SIEN_SEL    BIT(4)  // Interrupt on Selected or reselected
#define REG_SIEN_STO    BIT(5)  // Interrupt on SCSI bus timeout
#define REG_SIEN_FCMP   BIT(6)  // Interrupt on Function complete
#define REG_SIEN_PM     BIT(7)  // Interrupt on Unexpected Phase mismatch

#define REG_DIEN_DFE    BIT(7)  // DMA interrupt on FIFO empty
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
#define REG_DCNTL_STD   BIT(2)  // Start DMA operation (execute SCRIPT)
#define REG_DCNTL_LLM   BIT(3)  // Low level mode (no DMA or SCRIPTS)
#define REG_DCNTL_SSM   BIT(4)  // SCRIPTS single-step mode
#define REG_DCNTL_EA    BIT(5)  // Enable Ack
#define REG_DCNTL_CFD0  BIT(7)             // SCLK 16.67-25.00 MHz
#define REG_DCNTL_CFD1  BIT(6)             // SCLK 25.01-37.50 MHz
#define REG_DCNTL_CFD2  0                  // SCLK 37.50-50.00 MHz
#define REG_DCNTL_CFD3  (BIT(7) | BIT(6))  // SCLK 50.01-66.67 MHz

#define REG_DSTAT_IID   BIT(0)  // Illegal instruction
#define REG_DSTAT_WDT   BIT(1)  // Watchdog timeout
#define REG_DSTAT_SSI   BIT(3)  // SCRIPTS single-step interrupt
#define REG_DSTAT_ABRT  BIT(4)  // Abort condition
#define REG_DSTAT_BF    BIT(5)  // Bus fault
#define REG_DSTAT_DFE   BIT(7)  // DMA FIFO empty

#define REG_SCNTL1_ASEP BIT(2)  // Assert even SCSI data parity
#define REG_SCNTL1_RST  BIT(3)  // Assert reset on SCSI bus
#define REG_SCNTL1_ADB  BIT(6)  // Assert SCSI data bus (SODL/SOCL registers)

#define REG_SSTAT1_PAR  BIT(0)  // SCSI parity state
#define REG_SSTAT1_RST  BIT(1)  // SCSI bus reset is asserted

#define REG_CTEST4_FBL2 BIT(2)  // Send CTEST6 register to lane of the DMA FIFO
#define REG_CTEST4_SLBE BIT(4)  // SCSI loopback mode enable
#define REG_CTEST4_CDIS BIT(7)  // Cache burst disable

#define REG_CTEST5_DACK BIT(0)  // Data acknowledge (1=DMA acks SCSI DMA req)
#define REG_CTEST5_DREQ BIT(1)  // Data request (1=SCSI requests DMA transfer)
#define REG_CTEST5_DDIR BIT(3)  // DMA direction (1=SCSI->host, 0=host->SCSI)
#define REG_CTEST5_BBCK BIT(6)  // Decrement DBC by 1, 2, or 4
#define REG_CTEST5_ADCK BIT(7)  // Increment DNAD by 1, 2, or 4

#define REG_CTEST8_CLF  BIT(2)  // Clear DMA and SCSI FIFOs
#define REG_CTEST8_FLF  BIT(3)  // Flush DMA FIFO

#define NCR_FIFO_SIZE 16

#define AMIGA_BERR_DSACK 0x00de0000  // Bit7=1 for BERR on timeout, else DSACK
#define BERR_DSACK_SAVE() \
        uint8_t old_berr_dsack = *ADDR8(AMIGA_BERR_DSACK); \
        *ADDR8(AMIGA_BERR_DSACK) &= ~BIT(7);
#define BERR_DSACK_RESTORE() \
        *ADDR8(AMIGA_BERR_DSACK) = old_berr_dsack;

/* Modern stdint types */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef volatile APTR VAPTR;
typedef unsigned int   uint;

static void a4091_state_restore(int skip_reset);

static uint               runtime_flags = 0;
static const char * const expansion_library_name = "expansion.library";

typedef struct {
    struct Interrupt *local_isr;   // Temporary interrupt server
    uint32_t reg_addr;             // Base address of device registers
    volatile uint32_t intcount;    // Total interrupts
    volatile uint8_t ireg_istat;   // ISTAT captured by interrupt handler
    volatile uint8_t ireg_sien;    // SIEN captured by interrupt handler
    volatile uint8_t ireg_sstat0;  // SSTAT0 captured by interrupt handler
    volatile uint8_t ireg_dstat;   // DSTAT captured by interrupt handler
    uint8_t  card_owned;           // Task current owns device interrupts
    uint8_t  cleanup_installed;    // Cleanup on exit code installed
    uint32_t driver_isr_count;     // Number of driver ISRs captured
    uint32_t driver_task_count;    // Number of driver tasks captured
    struct List      driver_isrs;  // SCSI driver interrupt server list
    struct List      driver_rtask; // SCSI driver ready task list
    struct List      driver_wtask; // SCSI driver waiting task list
} a4091_save_t;

static a4091_save_t a4091_save;

extern BOOL __check_abort_enabled;

static void
check_break(void)
{
    if (SetSignal(0, 0) & SIGBREAKF_CTRL_C) {
        printf("^C Abort\n");
        a4091_state_restore(!(runtime_flags & FLAG_DEBUG));
        exit(1);
    }
}

static uint64_t
read_system_ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);  /* Measured latency is ~250us on A3000 A3640 */
    return ((uint64_t) (ds.ds_Days * 24 * 60 +
                        ds.ds_Minute) * 60 * TICKS_PER_SECOND + ds.ds_Tick);
}

static void
print_system_ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);  /* Measured latency is ~250us on A3000 A3640 */
    printf("d=%d m=%d t=%d\n", ds.ds_Days, ds.ds_Minute, ds.ds_Tick);
}

static uint64_t
read_system_ticks_sync(void)
{
    uint64_t stick = read_system_ticks();
    uint64_t tick;
    while ((tick = read_system_ticks()) == stick)
        ;
    return (tick);
}

/*
 * get_milli_ticks
 * ---------------
 * Returns the number of fractional ticks (thousandths) which have elapsed
 * past the specified tick. This is done by counting the number of OS
 * calls to DateStamp() before the tick counter rolls over. The resolution
 * is likely not accurate past the tenths place, as on an A4000T with A3640,
 * the Amiga can only do about 58 calls per tick.
 */
uint
get_milli_ticks(uint64_t ltick)
{
    uint64_t ctick;
    uint64_t etick;
    uint left_count = 0;
    uint full_count = 0;
    while ((ctick = read_system_ticks()) == ltick)
        left_count++;
    while ((etick = read_system_ticks()) == ctick)
        full_count++;

    if (left_count > full_count)
        left_count = full_count;
    return ((full_count * (ctick - ltick) - left_count) * 1000 / full_count);
}

static const char * const z2_config_sizes[] =
{
    "8 MB", "64 KB", "128 KB", "256 KB", "512KB", "1MB", "2MB", "4MB"
};

static const char * const z3_config_sizes[] =
{
    "16 MB", "32 MB", "64 MB", "128 MB", "256 MB", "512 MB", "1 GB", "RSVD"
};

static const char * const config_subsizes[] =
{
    "Same-as-Physical", "Automatically-sized", "64 KB", "128 KB",
    "256 KB", "512 KB", "1MB", "2MB",
    "4MB", "6MB", "8MB", "10MB", "12MB", "14MB", "Rsvd1", "Rsvd2"
};

static uint     flag_verbose = 1;     // Run in verbose mode
static uint32_t a4091_base;           // Base address for current card
static uint32_t a4091_rom_base;       // ROM base address
static uint32_t a4091_reg_base;       // Registers base address
static uint32_t a4091_switch_base;    // Switches base address

static uint8_t
get_rom(uint off)
{
    if (runtime_flags & FLAG_IS_A4000T) {
        return (0);  // No Zorro config for 53C710 in A4000T
    } else {
        /*
         * ROM byte bits 7:4 are in base[addr] 31:28
         * ROM byte bits 3:0 are in base[addr] 15:12
         */
        uint32_t hilo = *ADDR32(a4091_base + A4091_OFFSET_ROM + off * 4);
        return (((hilo >> 24) & 0xf0) | ((hilo >> 12) & 0xf));
    }
}

static uint8_t
get_creg(uint reg)
{
    uint8_t hi;
    uint8_t lo;
    if (runtime_flags & FLAG_IS_A4000T) {
        return (0);  // No Zorro config for 53C710 in A4000T
    } else {
        /*
         * CREG byte bits 7:4 are in base[addr + 0x000] 7:4
         * CREG byte bits 3:0 are in base[addr + 0x100] 7:4
         */
        hi = ~*ADDR8(a4091_base + A4091_OFFSET_AUTOCONFIG + reg * 4);
        lo = ~*ADDR8(a4091_base + A4091_OFFSET_AUTOCONFIG + reg * 4 + 0x100);
        return ((hi & 0xf0) | (lo >> 4));
    }
}

#if 0
static void
set_creg(uint32_t addr, uint reg, uint8_t value)
{
    *ADDR8(addr + A4091_OFFSET_AUTOCONFIG + reg * 4) = value;
    *ADDR8(addr + A4091_OFFSET_AUTOCONFIG + reg * 4 + 0x100) = value << 4;
}
#endif

static void
show_creg_value(uint reg, uint8_t value)
{
    printf("   %02x   %02x", reg, value);
}

static uint8_t
show_creg(uint reg)
{
    uint8_t value = get_creg(reg);
    show_creg_value(reg, value);
    return (value);
}

static int
autoconfig_reserved(uint reg)
{
    uint8_t value = get_creg(reg);
    if (value != 0x00) {
        show_creg_value(reg, value);
        printf(" Reserved: should be 0x00\n");
        return (1);
    }
    return (0);
}


static int
decode_autoconfig(void)
{
    uint8_t  value;
    uint32_t value32;
    int      rc = 0;
    int      is_z3 = 0;
    int      is_autoboot = 0;
    int      byte;
    const char * const *sizes = z2_config_sizes;

    printf("A4091 Autoconfig area\n"
           "  Reg Data Decode\n");
    value = ~show_creg(0x00);
    switch (value >> 6) {
        case 0:
        case 1:
            printf(" Zorro_Reserved");
            break;
        case 2:
            printf(" ZorroIII");
            is_z3 = 1;
            break;
        case 3:
            printf(" ZorroII");
            break;
    }
    if (value & BIT(5))
        printf(" Memory");
    if (is_z3 && (get_creg(0x08) & BIT(5)))
        sizes = z3_config_sizes;
    printf(" Size=%s", sizes[value & 0x7]);
    if (value & BIT(4)) {
        printf(" Autoboot");
        is_autoboot = 1;
    }
    if (value & BIT(3))
        printf(" Link-to-next");
    printf("\n");

    printf(" Product=0x%02x\n", show_creg(0x01) & 0xff);

    value = show_creg(0x02);
    if (is_z3) {
        if (value & BIT(7)) {
            printf(" Device-Memory");
            rc++;  // Unexpected for A4091
        } else {
            printf(" Device-IO");
        }
    } else {
        rc++;  // Unexpected for A4091
        if (value & BIT(7))
            printf(" Fit-ZorroII");
        else
            printf(" Fit-anywhere");
    }
    if (value & BIT(5))
        printf(" NoShutup");
    if (is_z3 && ((value & BIT(4)) == 0))
        printf(" Invalid_RSVD");

    if (value & BIT(5))
        printf(" SizeExt");
    printf(" %s\n", config_subsizes[value & 0x0f]);

    if (autoconfig_reserved(0x03))
        rc = 1;

    value32 = show_creg(0x04) << 8;
    printf(" Mfg Number high byte\n");
    value32 |= show_creg(0x05);
    printf(" Mfg Number low byte    Manufacturer=0x%04x\n", value32);

    value32 = 0;
    for (byte = 0; byte < 4; byte++) {
        value32 <<= 8;
        value32 |= show_creg(0x06 + byte);
        printf(" Serial number byte %d", byte);
        if (byte == 3)
            printf("   Serial=0x%08x", value32);
        printf("\n");
    }

    if (is_autoboot) {
        value32 = show_creg(0x10) << 8;
        printf(" Option ROM vector high\n");
        value32 |= show_creg(0x11);
        printf(" Option ROM vector low  Offset=0x%04x\n", value32);
    }
    for (byte = 0x0c; byte <= 0x0f; byte++)
        rc += autoconfig_reserved(byte);
    for (byte = 0x14; byte <= 0x31; byte++)
        rc += autoconfig_reserved(byte);

    return (rc);
}

typedef const char * const bitdesc_t;

static bitdesc_t bits_scntl0[] = {
    "TRG", "AAP", "EPG", "EPC", "WATN/", "START", "ARB0", "ARB1"
};
static bitdesc_t bits_scntl1[] = {
    "RES0", "RES1", "AESP", "RST", "CON", "FSR", "ADB", "EXC"
};
static bitdesc_t bits_sien[] = {
    "PAR", "RST/", "UDC", "SGE", "SEL", "STO", "FCMP", "M/A"
};
static bitdesc_t bits_sbcl[] = {
    "I/O", "C/D", "MSG", "ATN", "SEL", "BSY", "ACK", "REQ"
};
static bitdesc_t bits_dstat[] = {
    "IID", "WTD", "SIR", "SSI", "ABRT", "RF", "RES6", "DFE"
};
static bitdesc_t bits_sstat0[] = {
    "PAR", "RST/", "UDC", "SGE", "SEL", "STO", "FCMP", "M/A"
};
static bitdesc_t bits_sstat1[] = {
    "SDP/", "RST/", "WOA", "LOA", "AIP", "OLF", "ORF", "ILF"
};
static bitdesc_t bits_sstat2[] = {
    "I/O", "C/D", "MSG", "SDP", "FF0", "FF1", "FF2", "FF3"
};
static bitdesc_t bits_ctest0[] = {
    "DDIR", "RES1", "ERF", "HSC", "EAN", "GRP", "BTD", "RES7"
};
static bitdesc_t bits_ctest2[] = {
    "DACK", "DREQ", "TEOP", "DFP", "SFP", "SOFF", "SIGP", "RES7"
};
static bitdesc_t bits_ctest4[] = {
    "FBL0", "FBL1", "FBL2", "SFWR", "SLBE", "SZM", "ZMOD", "MUX"
};
static bitdesc_t bits_ctest5[] = {
    "DACK", "DREQ", "EOP", "DDIR", "MASR", "ROFF", "BBCK", "ADCK"
};
static bitdesc_t bits_ctest7[] = {
    "DIFF", "TT1", "EVP", "DFP", "NOTIME", "SC0", "SC1", "CDIS"
};
static bitdesc_t bits_istat[] = {
    "DIP", "SIP", "RSV2", "CON", "RSV4", "SIOP", "RST", "ABRT"
};
static bitdesc_t bits_ctest8[] = {
    "SM", "FM", "CLF", "FLF", "V0", "V1", "V2", "V3"
};
static bitdesc_t bits_dmode[] = {
    "MAN", "U0", "FAM", "PD", "FC1", "FC2", "BL0", "BL1"
};
static bitdesc_t bits_dien[] = {
    "HD", "WTD", "SIR", "SSI", "ABRT", "BF", "RES6", "RES7"
};
static bitdesc_t bits_dcntl[] = {
    "COM", "FA", "STD", "LLM", "SSM", "EA", "CF0", "CF1"
};

typedef struct {
    uint8_t            reg_loc;
    uint8_t            reg_size;     // size in bytes
    uint8_t            show;         // safe to read/display this register
    uint8_t            pad;          // structure padding
    const char         reg_name[8];  // Register name
    const char * const reg_desc;     // Long description
    bitdesc_t         *reg_bits;     // Individual bit names
} ncr_regdefs_t;

static const ncr_regdefs_t ncr_regdefs[] =
{
    { 0x03, 1, 1, 0, "SCNTL0",   "SCSI control 0", bits_scntl0 },
    { 0x02, 1, 1, 0, "SCNTL1",   "SCSI control 1", bits_scntl1 },
    { 0x01, 1, 1, 0, "SDID",     "SCSI destination ID" },
    { 0x00, 1, 1, 0, "SIEN",     "SCSI IRQ enable", bits_sien },
    { 0x07, 1, 1, 0, "SCID",     "SCSI chip ID" },
    { 0x06, 1, 1, 0, "SXFER",    "SCSI transfer" },
    { 0x05, 1, 1, 0, "SODL",     "SCSI output data latch" },
    { 0x04, 1, 1, 0, "SOCL",     "SCSI output control latch", bits_sbcl },
    { 0x0b, 1, 1, 0, "SFBR",     "SCSI first byte received" },
    { 0x0a, 1, 1, 0, "SIDL",     "SCSI input data latch" },
    { 0x09, 1, 1, 0, "SBDL",     "SCSI bus data lines" },
    { 0x08, 1, 1, 0, "SBCL",     "SCSI bus control lines", bits_sbcl },
    { 0x0f, 1, 1, 0, "DSTAT",    "DMA status", bits_dstat },
    { 0x0e, 1, 1, 0, "SSTAT0",   "SCSI status 0", bits_sstat0 },
    { 0x0d, 1, 1, 0, "SSTAT1",   "SCSI status 1", bits_sstat1 },
    { 0x0c, 1, 1, 0, "SSTAT2",   "SCSI status 2", bits_sstat2 },
    { 0x10, 4, 1, 0, "DSA",      "Data structure address" },
    { 0x17, 1, 1, 0, "CTEST0",   "Chip test 0", bits_ctest0 },
    { 0x16, 1, 1, 0, "CTEST1",   "Chip test 1 7-4=FIFO_Empty 3-0=FIFO_Full" },
    { 0x15, 1, 1, 0, "CTEST2",   "Chip test 2", bits_ctest2 },
    { 0x14, 1, 0, 0, "CTEST3",   "Chip test 3 SCSI FIFO" },
    { 0x1b, 1, 1, 0, "CTEST4",   "Chip test 4", bits_ctest4 },
    { 0x1a, 1, 1, 0, "CTEST5",   "Chip test 5", bits_ctest5 },
    { 0x19, 1, 0, 0, "CTEST6",   "Chip test 6 DMA FIFO" },
    { 0x18, 1, 1, 0, "CTEST7",   "Chip test 7", bits_ctest7 },
    { 0x1c, 4, 1, 0, "TEMP",     "Temporary Stack" },
    { 0x23, 1, 1, 0, "DFIFO",    "DMA FIFO" },
    { 0x22, 1, 1, 0, "ISTAT",    "Interrupt Status", bits_istat },
    { 0x21, 1, 1, 0, "CTEST8",   "Chip test 8", bits_ctest8 },
    { 0x20, 1, 1, 0, "LCRC",     "Longitudinal parity" },
    { 0x25, 3, 1, 0, "DBC",      "DMA byte counter" },
    { 0x24, 1, 1, 0, "DCMD",     "DMA command" },
    { 0x28, 4, 1, 0, "DNAD",     "DMA next address for data" },
    { 0x2c, 4, 1, 0, "DSP",      "DMA SCRIPTS pointer" },
    { 0x30, 4, 1, 0, "DSPS",     "DMA SCRIPTS pointer save" },
    { 0x34, 4, 1, 0, "SCRATCH",  "General purpose scratch pad" },
    { 0x3b, 1, 1, 0, "DMODE",    "DMA mode", bits_dmode },
    { 0x3a, 1, 1, 0, "DIEN",     "DMA interrupt enable", bits_dien },
    { 0x39, 1, 1, 0, "DWT",      "DMA watchdog timer" }, // No support in FS-UAE
    { 0x38, 1, 1, 0, "DCNTL",    "DMA control", bits_dcntl },
    { 0x3c, 4, 1, 0, "ADDER",    "Sum output of internal adder" },
};

static uint8_t
get_ncrreg8_noglob(uint32_t a4091_regs, uint reg)
{
    uint8_t value;
    value = *ADDR8(a4091_regs + reg);
    return (value);
}

static void
set_ncrreg8_noglob(uint32_t a4091_regs, uint reg, uint8_t value)
{
    *ADDR8(a4091_regs + 0x40 + reg) = value;
}

static uint8_t
get_ncrreg8(uint reg)
{
    uint8_t value = *ADDR8(a4091_reg_base + reg);
    if (runtime_flags & FLAG_DEBUG)
        printf("[%08x] R %02x\n", a4091_reg_base + reg, value);
    return (value);
}

static uint32_t
get_ncrreg32(uint reg)
{
#if 1
    uint32_t value = *ADDR32(a4091_reg_base + reg);
#else
    /*
     * Work around 68030 cache line allocation bug.
     * Not sure this is needed, so the code is disabled at this time.
     */
    APTR addr = (APTR) (a4091_reg_base + reg);
    uint32_t value;
    ULONG buf_handled = 4;
    CachePreDMA(addr, &buf_handled, DMA_ReadFromRAM);
    value = *ADDR32(a4091_reg_base + reg);
    CachePostDMA(addr, &buf_handled, DMA_ReadFromRAM);
#endif
    if (runtime_flags & FLAG_DEBUG)
        printf("[%08x] R %08x\n", a4091_reg_base + reg, value);
    return (value);
}

static uint32_t
get_ncrreg32b(uint reg)
{
    uint32_t value = (*ADDR8(a4091_reg_base + reg + 0) << 24) |
                     (*ADDR8(a4091_reg_base + reg + 1) << 16) |
                     (*ADDR8(a4091_reg_base + reg + 2) << 8) |
                     (*ADDR8(a4091_reg_base + reg + 3));
    if (runtime_flags & FLAG_DEBUG)
        printf("[%08x] R %08x\n", a4091_reg_base + reg, value);
    return (value);
}

/* Write at shadow register (+0x40) to avoid 68030 write-allocate bug */
static void
set_ncrreg8(uint reg, uint8_t value)
{
    if (runtime_flags & FLAG_DEBUG)
        printf("[%08x] W %02x\n", a4091_reg_base + reg, value);
    *ADDR8(a4091_reg_base + 0x40 + reg) = value;
}

static void
set_ncrreg32(uint reg, uint32_t value)
{
    if (runtime_flags & FLAG_DEBUG)
        printf("[%08x] W %08x\n", a4091_reg_base + reg, value);
    *ADDR32(a4091_reg_base + 0x40 + reg) = value;
}

static void
set_ncrreg32b(uint reg, uint32_t value)
{
    if (runtime_flags & FLAG_DEBUG)
        printf("[%08x] W %08x\n", a4091_reg_base + reg, value);
    *ADDR8(a4091_reg_base + 0x40 + reg + 0) = value >> 24;
    *ADDR8(a4091_reg_base + 0x40 + reg + 1) = value >> 16;
    *ADDR8(a4091_reg_base + 0x40 + reg + 2) = value >> 8;
    *ADDR8(a4091_reg_base + 0x40 + reg + 3) = value;
}

/*
 * access_timeout
 * --------------
 * Returns non-zero if the number of ticks has elapsed since the specified
 * tick_start.
 */
static int
access_timeout(const char *msg, uint32_t ticks, uint64_t tick_start)
{
    uint64_t tick_end = read_system_ticks();

    if (tick_end < tick_start) {
        printf("Invalid time comparison: %08x:%08x < %08x:%08x\n",
               (uint32_t) (tick_end >> 32), (uint32_t) tick_end,
               (uint32_t) (tick_start >> 32), (uint32_t) tick_start);
        return (FALSE);  /* Should not occur */
    }

    /* Signed integer compare to avoid wrap */
    if ((int) (tick_end - tick_start) > (int) ticks) {
        uint64_t diff;
        printf("%s: %d ticks", msg, (uint32_t) (tick_end - tick_start));
        diff = tick_end - tick_start;
        if (diff > TICKS_PER_SECOND * 10) {
            printf(": bug? %08x%08x %08x%08x\n",
                   (uint32_t) (tick_start >> 32), (uint32_t) tick_start,
                   (uint32_t) (tick_end >> 32), (uint32_t) tick_end);
            print_system_ticks();
        }
        printf("\n");
        return (TRUE);
    }
    return (FALSE);
}

/*
 * a4091_reset
 * -----------
 * Resets the A4091's 53C710 SCSI controller.
 */
static void
a4091_reset(void)
{
    if (runtime_flags & FLAG_IS_A4000T)
        set_ncrreg8(REG_DCNTL, REG_DCNTL_EA);   // Enable Ack: allow reg writes
    set_ncrreg8(REG_ISTAT, REG_ISTAT_RST);  // Reset
    (void) get_ncrreg8(REG_ISTAT);          // Push out write

    set_ncrreg8(REG_ISTAT, 0);              // Clear reset
    (void) get_ncrreg8(REG_ISTAT);          // Push out write
    Delay(1);

    /* SCSI Core clock (37.51-50 MHz) */
    if (runtime_flags & FLAG_IS_A4000T)
        set_ncrreg8(REG_DCNTL, REG_DCNTL_COM | REG_DCNTL_CFD2 | REG_DCNTL_EA);
    else
        set_ncrreg8(REG_DCNTL, REG_DCNTL_COM | REG_DCNTL_CFD2);

    set_ncrreg8(REG_SCID, BIT(7));          // Set SCSI ID

    set_ncrreg8(REG_DWT, 0xff);             // 25MHz DMA timeout: 640ns * 0xff

    const int burst_mode = 8;
    switch (burst_mode) {
        default:
        case 1:
            /* 1-transfer burst, FC = 101 -- works on A3000 */
            set_ncrreg8(REG_DMODE, REG_DMODE_BLE0 | REG_DMODE_FC2);
            break;
        case 2:
            /* 2-transfer burst, FC = 101 */
            set_ncrreg8(REG_DMODE, REG_DMODE_BLE1 | REG_DMODE_FC2);
            break;
        case 4:
            /* 4-transfer burst, FC = 101 */
            set_ncrreg8(REG_DMODE, REG_DMODE_BLE2 | REG_DMODE_FC2);
            break;
        case 8:
            /* 8-transfer burst, FC = 101 */
            set_ncrreg8(REG_DMODE, REG_DMODE_BLE3 | REG_DMODE_FC2);
            break;
    }

    if ((runtime_flags & FLAG_IS_A4000T) == 0) {
        /* Disable cache line bursts */
        set_ncrreg8(REG_CTEST7, get_ncrreg8(REG_CTEST7) | REG_CTEST4_CDIS);
    }

    /* Disable interrupts */
    set_ncrreg8(REG_DIEN, 0);
}

#if 0
/*
 * a4091_abort
 * -----------
 * Abort the current SCRIPTS operation, stopping the SCRIPTS processor.
 */
static void
a4091_abort(void)
{
    uint8_t istat;
    uint64_t tick_start;

    istat = get_ncrreg8(REG_ISTAT);
    set_ncrreg8(REG_ISTAT, istat | REG_ISTAT_ABRT);
    (void) get_ncrreg8(REG_ISTAT);

    tick_start = read_system_ticks();
    while ((get_ncrreg8(REG_DSTAT) & REG_DSTAT_ABRT) == 0) {
        if (access_timeout("DSTAT_ABRT timeout", 2, tick_start))
            break;
    }
}
#endif

/*
 * a4091_irq_handler
 * -----------------
 * Handle interrupts from the 53C710 SCSI controller
 */
LONG
a4091_irq_handler(void)
{
    register a4091_save_t *save asm("a1");

    uint8_t istat = get_ncrreg8_noglob(save->reg_addr, REG_ISTAT);

    if (istat == 0) {
        return (0);
    }

    if (istat & REG_ISTAT_ABRT)
        set_ncrreg8_noglob(save->reg_addr, REG_ISTAT, 0);

    if ((istat & (REG_ISTAT_DIP | REG_ISTAT_SIP)) != 0) {
        uint prev_istat = save->ireg_istat;
        /*
         * If ISTAT_SIP is set, read SSTAT0 register to determine cause
         * If ISTAT_DIP is set, read DSTAT register to determine cause
         */
        save->ireg_istat  = istat;
        save->ireg_sien   = get_ncrreg8_noglob(save->reg_addr, REG_SIEN);
        save->ireg_sstat0 = get_ncrreg8_noglob(save->reg_addr, REG_SSTAT0);
        save->ireg_dstat  = get_ncrreg8_noglob(save->reg_addr, REG_DSTAT);

        save->intcount++;

        if (prev_istat == 0) {
            return (1);  // Handled
        }
    }
    return (0);  // Not handled
}

static void
a4091_add_local_irq_handler(void)
{
    if (a4091_save.local_isr == NULL) {
        a4091_save.intcount = 0;
        a4091_save.local_isr = AllocMem(sizeof (*a4091_save.local_isr),
                                        MEMF_CLEAR | MEMF_PUBLIC);
        a4091_save.local_isr->is_Node.ln_Type = NT_INTERRUPT;
        /*
         * set higher priority so that the test utility can steal interrupts
         * from the driver when it is running.
         */
        a4091_save.local_isr->is_Node.ln_Pri  = A4091_INTPRI + 1;
        a4091_save.local_isr->is_Node.ln_Name = "A4091 test";
        a4091_save.local_isr->is_Data         = &a4091_save;
        a4091_save.local_isr->is_Code         = (void (*)()) a4091_irq_handler;

        if (runtime_flags & FLAG_DEBUG)
            printf("my irq handler=%x %x\n",
                   (uint32_t) &a4091_save, (uint32_t) a4091_save.local_isr);
        AddIntServer(A4091_IRQ, a4091_save.local_isr);
    }
}

static void
a4091_remove_local_irq_handler(void)
{
    if (a4091_save.local_isr != NULL) {
        RemIntServer(A4091_IRQ, a4091_save.local_isr);
        FreeMem(a4091_save.local_isr, sizeof (*a4091_save.local_isr));
        a4091_save.local_isr = NULL;
    }
}

static char *
GetNodeName(struct Node *node)
{
    if (node == NULL)
        return ("");
    return ((node->ln_Name == NULL) ? "(missing)" : node->ln_Name);
}

static int
a4091_show_or_disable_driver_irq_handler(int disable, int show)
{
    struct IntVector *iv    = &SysBase->IntVects[A4091_IRQ];
    struct List      *slist = iv->iv_Data;
    struct Interrupt *s;
    uint   count = 0;
    const char *suspend_name = "";

    if (EMPTY(slist))
        return (0);

    Disable();
    for (s = FIRST(slist); NEXTINT(s); s = NEXTINT(s)) {
        const char *name = GetNodeName((struct Node *) s);
        if (runtime_flags & FLAG_IS_A4000T) {
            if ((strcmp(name, "NCR SCSI") != 0) &&
                (strcmp(name, "A4091 test") != 0))
                continue;  // No match
        } else {
            if ((strcmp(name, "NCR SCSI") != 0) &&
                (strcmp(name, "A4091 test") != 0) &&
                (strcmp(name, "a4091.device") != 0))
                continue;  // No match
            if (((uint32_t) s->is_Code >= 0x00f00000) &&
                ((uint32_t) s->is_Code <= 0x00ffffff))
                continue;  // Match, but driver is in Kickstart ROM
        }
        if (s == a4091_save.local_isr)
            continue;  /* Don't show or clobber our own ISR handler */

        count++;
        if (show || (runtime_flags & FLAG_DEBUG)) {
            Enable();
            printf("  INT%x \"%s\" %08x %08x %08x\n",
                   A4091_IRQ, name, (uint32_t) s->is_Code,
                   (uint32_t) s->is_Data, (uint32_t) &s->is_Node);
            Disable();
        }
        if (disable) {
            /* Found A4091 SCSI driver's interrupt server -- disable it */
            struct Node *node = &s->is_Node;
            RemIntServer(A4091_IRQ, (struct Interrupt *) node);
            AddHead(&a4091_save.driver_isrs, node);
            a4091_save.driver_isr_count++;
            suspend_name = name;
            break;
        }
    }
    Enable();
    if ((count > 0) && disable) {
        printf("%s driver IRQ handler%s %s\n",
               (disable & 2) ? "Killed" : "Suspended",
               (count > 1) ? "s" : "", suspend_name);
    }
    return (count);
}

static void
a4091_enable_driver_irq_handler(void)
{
    if (a4091_save.driver_isr_count > 0) {
        struct Node *node;
        struct Node *next;
        if (runtime_flags & FLAG_DEBUG)
            printf("Restoring A4091 driver IRQ handler\n");
        for (node = a4091_save.driver_isrs.lh_Head;
             node->ln_Succ != NULL; node = next) {
            next = node->ln_Succ;
            AddIntServer(A4091_IRQ, (struct Interrupt *) node);
        }
        a4091_save.driver_isr_count = 0;
    }
}

static void
a4091_enable_driver_task(void)
{
    if (a4091_save.driver_task_count > 0) {
        struct Node *node;
        struct Node *next;

        if (runtime_flags & FLAG_DEBUG)
            printf("Restoring A4091 SCSI driver task\n");
        Disable();
        for (node = a4091_save.driver_rtask.lh_Head;
             node->ln_Succ != NULL; node = next) {
            next = node->ln_Succ;
            AddHead(&SysBase->TaskReady, node);
        }
        for (node = a4091_save.driver_wtask.lh_Head;
             node->ln_Succ != NULL; node = next) {
            next = node->ln_Succ;
            AddHead(&SysBase->TaskWait, node);
        }
        Enable();
        a4091_save.driver_task_count = 0;
    }
}

static int
is_handler_task(const char *name)
{
    if (runtime_flags & FLAG_IS_A4000T) {
        if (strcmp(name, "A4000T SCSI handler") != 0)
            return (0);
    } else {
        if ((strcmp(name, "A3090 SCSI handler") != 0) &&
            (strcmp(name, "a4091.device") != 0))
            return (0);
    }
    return (1);
}

static int
a4091_show_or_disable_driver_task(int disable, int show)
{
    struct Node *node;
    struct Node *next;
    uint count = 0;
    uint pass;
    const char *suspend_name = "";

    if (show) {
        Forbid();
        for (pass = 0; pass < 2; pass++) {
            if (pass == 0)
                node = SysBase->TaskReady.lh_Head;
            else
                node = SysBase->TaskWait.lh_Head;

            for (; node->ln_Succ != NULL; node = next) {
                struct Task *task = (struct Task *) node;
                const char *name = GetNodeName((struct Node *) node);
                next = node->ln_Succ;

                if (is_handler_task(name)) {
                    printf("  Task \"%s\" %08x [%08x] in %s queue\n",
                           name, (uint32_t) node, (uint32_t) task->tc_SPReg,
                           (pass == 0) ? "Ready" : "Wait");
                    count++;
                }
            }
        }
        Permit();
    }

    if (disable) {
        uint scount = 0;
        Disable();
        for (pass = 0; pass < 2; pass++) {
            if (pass == 0)
                node = SysBase->TaskReady.lh_Head;
            else
                node = SysBase->TaskWait.lh_Head;

            for (; node->ln_Succ != NULL; node = next) {
                const char *name = GetNodeName((struct Node *) node);
                next = node->ln_Succ;

                if (is_handler_task(name)) {
                    Remove(node);
                    if (pass == 0)
                        AddHead(&a4091_save.driver_rtask, node);
                    else
                        AddHead(&a4091_save.driver_wtask, node);
                    a4091_save.driver_task_count++;
                    suspend_name = name;
                    scount++;
                }
            }
        }
        Enable();
        if (scount > 0) {
            printf("%s driver task%s %s\n",
                   (disable & 2) ? "Killed" : "Suspended",
                   (scount > 1) ? "s" : "", suspend_name);
        }
        count = scount;
    }
    return (count);
}

static void
a4091_remove_driver_from_devlist(void)
{
    /*
     * XXX: I've not found a reliable way to locate the "correct" device
     *      to remove from the devlist.
     *
     *      devlist entries don't have an association with the physical
     *      device being handled.
     *
     *      The configdevlist doesn't appear to have a valid a pointer
     *      to the specific driver in use (at least in my A3000 with 3.2.1,
     *      I got 0x00000000 for cd_Driver).
     */
#if 0
    struct Node *node;
    struct Node *next;

    Forbid();
    /* Does printing within Forbid() break the forbidden state? */
    for (node = SysBase->DeviceList.lh_Head;
         node->ln_Succ != NULL; node = next) {
        const char *name = GetNodeName((struct Node *) node);
        next = node->ln_Succ;
        if (strstr(name, "scsi.device") != NULL) {
            struct Library *lib = (struct Library *) node;
            printf("found %p %s %s\n",
                   node, node->ln_Name, (char *)lib->lib_IdString);
        }
    }
    Permit();


    struct Library   *ExpansionBase;
    struct ConfigDev *cdev  = NULL;
    uint32_t          addr  = -1;  /* Default to not found */
    int               count = 0;
    uint32_t pos = 0;

    if ((ExpansionBase = OpenLibrary(expansion_library_name, 0)) == 0) {
        printf("Could not open %s\n", expansion_library_name);
        return;
    }

    do {
        cdev = FindConfigDev(cdev, ZORRO_MFG_COMMODORE, ZORRO_PROD_A4091);
        if (cdev != NULL) {
            count++;
            if ((pos == 0) || (pos == count)) {
                addr = (uint32_t) cdev->cd_BoardAddr;
                printf("found %08x %p %p\n", addr, cdev, cdev->cd_Driver);
                break;
            }
        }
    } while (cdev != NULL);

    CloseLibrary(ExpansionBase);
#endif
}

/*
 * attempt_driver_expunge
 * ----------------------
 * If the A4091 driver appears in the interrupt list or task list, this
 * function will attempt to force an expunge of all drivers and libraries
 * which have a zero open count.
 */
static void
attempt_driver_expunge(void)
{
    int count;
    for (count = 0; count < 4; count++) {
        if ((a4091_show_or_disable_driver_irq_handler(0, 0) +
             a4091_show_or_disable_driver_task(0, 0)) == 0)
            break;
        (void) AllocMem(0x7fff0000, MEMF_PUBLIC);
        Delay(20);
    }
}

static int
kill_driver(void)
{
    /* XXX: Should first unmount filesystems served by the driver */
    attempt_driver_expunge();
    a4091_reset();
    a4091_show_or_disable_driver_irq_handler(2, 1);
    a4091_show_or_disable_driver_task(2, 1);
    a4091_remove_driver_from_devlist();
    a4091_save.driver_isr_count  = 0;  // Prevent ISRs from being reinstalled
    a4091_save.driver_task_count = 0;  // Prevent Tasks from being reinstalled
    return (0);
}

/*
 * a4091_state_restore
 * -------------------
 * Resets the A4091's 53C710, restores A4091 state, and then disables the
 * private interrupt handler.
 */
static void
a4091_state_restore(int skip_reset)
{
    if (a4091_save.card_owned) {
        a4091_save.card_owned = 0;
        set_ncrreg8(REG_DIEN, 0);
        set_ncrreg8(REG_ISTAT, 0);
        if (skip_reset == 0)
            a4091_reset();
        a4091_enable_driver_irq_handler();
        a4091_remove_local_irq_handler();
        a4091_enable_driver_task();

        if ((a4091_save.intcount != 0) & (runtime_flags & FLAG_DEBUG)) {
            printf("Interrupt count=%d "
                   "ISTAT=%02x SSTAT0=%02x DSTAT=%02x SIEN=%02x\n",
                    a4091_save.intcount, a4091_save.ireg_istat,
                    a4091_save.ireg_sstat0, a4091_save.ireg_dstat,
                    a4091_save.ireg_sien);
        }
    }
}


/*
 * a4091_cleanup
 * -------------
 * Called at program exit
 */
static void
a4091_cleanup(void)
{
    a4091_state_restore(!(runtime_flags & FLAG_DEBUG));
}

/*
 * a4091_state_takeover
 * --------------------
 * Sets up a private interrupt handler and captures A4091 state.
 */
static void
a4091_state_takeover(int flag_suspend)
{
    if (a4091_save.cleanup_installed == 0) {
        a4091_save.cleanup_installed = 1;
        atexit(a4091_cleanup);
    }
    if (a4091_save.card_owned == 0) {
        a4091_save.card_owned = 1;

        /*
         * Save procedure:
         * 1. Capture interrupts
         * 2. Capture SCRIPTS processor run/stop state.
         * 3. If running, first suspend the SCRIPTS processor by putting
         *    it in single step mode. Wait for it to stop.
         */
        a4091_add_local_irq_handler();
        if (flag_suspend) {
            a4091_show_or_disable_driver_irq_handler(1, 0);
            a4091_show_or_disable_driver_task(1, 0);
        }

#if 0
        /* Stop SCRIPTS processor */
        if (!is_running_in_uae()) {
            /* Below line causes segfault or hang of FS-UAE */
            // XXX: How can we tell if the SCRIPTS processor is running?
            printf("Stopping SIOP\n");
            set_ncrreg8(REG_ISTAT, REG_ISTAT_ABRT);
            (void) get_ncrreg8(REG_ISTAT);

            tick_start = read_system_ticks();
            while ((get_ncrreg8(REG_DSTAT) & REG_DSTAT_ABRT) == 0) {
                if (access_timeout("DSTAT_ABRT timeout", 2, tick_start))
                    break;
            }
        }

        set_ncrreg8(REG_DCNTL, get_ncrreg8(REG_DCNTL) | REG_DCNTL_SSM);
        (void) get_ncrreg8(REG_DCNTL);

        tick_start = read_system_ticks();
        while ((a4091_save.ireg_istat & REG_ISTAT_RST) == 0) {
            if (access_timeout("ISTAT_RST timeout", 2, tick_start))
                break;
        }
#endif

        /* Soft reset 53C710 SCRIPTS processor (SIOP) */
        if (runtime_flags & FLAG_DEBUG)
            printf("Soft resetting SIOP\n");
        a4091_reset();
    }
}


/*
 * To perform a read from memory, make the destination address be
 * within the 53C710's address space. To perform a write to memory,
 * make the source address be within the 53C710. If both addresses
 * are in system memory, then the 53C710 functions as a high-speed
 * DMA copy peripheral.
 *
 * 0x98080000 = Transfer Control, Opcode=011 (Interrupt)
 * 0xc8080000 = Memory-to-memory copy
 */
__attribute__((aligned(16)))
uint32_t dma_mem_move_script[] = {
    0xc0000000,              // Memory Move command: lower 24 bits are length
    0x00000000,              // Source address (DSPS)
    0x00000000,              // Destination address (TEMP)
    0x98080000, 0x00000000,  // Transfer Control Opcode=011 (Interrupt and stop)
};

__attribute__((aligned(16)))
uint32_t dma_mem_move_script_quad[] = {
    0xc0000000,              // Memory Move command: lower 24 bits are length
    0x00000000,              // Source address (DSPS)
    0x00000000,              // Destination address (TEMP)
    0xc0000000,              // Memory Move command: lower 24 bits are length
    0x00000000,              // Source address (DSPS)
    0x00000000,              // Destination address (TEMP)
    0xc0000000,              // Memory Move command: lower 24 bits are length
    0x00000000,              // Source address (DSPS)
    0x00000000,              // Destination address (TEMP)
    0xc0000000,              // Memory Move command: lower 24 bits are length
    0x00000000,              // Source address (DSPS)
    0x00000000,              // Destination address (TEMP)
    0x98080000, 0x00000000,  // Transfer Control Opcode=011 (Interrupt and stop)
};

__attribute__((aligned(16)))
uint32_t script_write_to_reg[] = {
    /*
     * Command
     *
     * Bits 31-30 01       = IO
     * Bits 29-27 111      = Read-Modify-Write
     * Bits 26-25 00       = Immediate data to destination register
     * Bit  24    0        = Carry Enable
     * Bits 23-22 00       = Reserved
     * Bits 21-16 111000   = A5-A0 = SCRATCH (0x34)
     * Bits 15-8  00000000 = Immediate data
     * Bits 7-0   00000000 = Reserved
     */
    0x78340000,              // Write immediate data to register
    0x98080000, 0x00000000,  // Transfer Control Opcode=011 (Interrupt and stop)
};

static void
dma_clear_istat(void)
{
    uint8_t istat;

    /* Clear pending interrupts */
    while (((istat = get_ncrreg8(REG_ISTAT)) & 0x03) != 0) {
        if (istat & REG_ISTAT_SIP)
            (void) get_ncrreg8(REG_SSTAT0);

        if (istat & REG_ISTAT_DIP)
            (void) get_ncrreg8(REG_DSTAT);

        if (istat & (REG_ISTAT_RST | REG_ISTAT_ABRT))
            set_ncrreg8(REG_ISTAT, 0);

        if (istat & (REG_ISTAT_DIP | REG_ISTAT_SIP))
            Delay(1);
    }
}

static int
execute_script(uint32_t *script, ULONG script_len)
{
    int rc = 0;
    int count = 1;
    uint8_t istat;
    uint8_t dstat;
    uint64_t tick_start;

    a4091_save.ireg_istat = 0;

    tick_start = read_system_ticks();

    CachePreDMA((APTR) script, &script_len, DMA_ReadFromRAM);
    /* Enable interrupts and start script */
    set_ncrreg8(REG_DIEN, REG_DIEN_ILD | REG_DIEN_WTD | REG_DIEN_SIR |
                REG_DIEN_SSI | REG_DIEN_ABRT | REG_DIEN_BF);
    set_ncrreg32(REG_DSP, (uint32_t) script);

    while (1) {
        istat = a4091_save.ireg_istat;
        if (istat & (REG_ISTAT_ABRT | REG_ISTAT_DIP)) {
            dstat = a4091_save.ireg_dstat;
            if (runtime_flags & FLAG_DEBUG)
                printf("Got DMA completion interrupt %02x %02x\n",
                       istat, dstat);
            goto got_dstat;
        } else if ((istat != 0) && (runtime_flags & FLAG_DEBUG)) {
            a4091_save.ireg_istat = 0;
            printf("istat=%02x\n", istat);
        }
        if ((count & 0xff) == 0) {
            uint8_t istat = get_ncrreg8(REG_ISTAT);
            if (istat & (REG_ISTAT_ABRT | REG_ISTAT_DIP)) {
                dstat = get_ncrreg8(REG_DSTAT);
                if (runtime_flags & FLAG_DEBUG)
                    printf("Got DMA polled completion %02x %02x\n",
                           istat, dstat);
got_dstat:
                if (dstat & (REG_DSTAT_BF | REG_DSTAT_ABRT | REG_DSTAT_WDT)) {
                    printf("[");
                    if (dstat & REG_DSTAT_BF)
                        printf("Bus fault ");
                    if (dstat & REG_DSTAT_ABRT)
                        printf("Abort ");
                    if (dstat & REG_DSTAT_WDT)
                        printf("Watchdog timeout ");
                    printf("%02x] ", dstat & ~REG_DSTAT_DFE);
                    rc = 1;
                    goto fail;
                }
                break;
            } else if ((istat != 0) && (runtime_flags & FLAG_DEBUG)) {
                printf("pistat=%02x\n", istat);
            }
        }

        if (((count & 0xff) == 0) &&
            access_timeout("SIOP timeout", 30, tick_start)) {
            uint8_t istat = get_ncrreg8(REG_ISTAT);
            dstat = get_ncrreg8(REG_DSTAT);
            dstat = get_ncrreg8(REG_DSTAT);
            printf("ISTAT=%02x %02x DSTAT=%02x %02x ",
                   a4091_save.ireg_istat, istat,
                   a4091_save.ireg_dstat, dstat);
            if (dstat & REG_DSTAT_BF)
                printf("Bus fault ");
            if (dstat & REG_DSTAT_ABRT)
                printf("Abort ");
            if (dstat & REG_DSTAT_WDT)
                printf("Watchdog timeout ");
            printf("SSTAT0=%02x SSTAT1=%02x SSTAT2=%02x\n",
                   get_ncrreg8(REG_SSTAT0), get_ncrreg8(REG_SSTAT1),
                   get_ncrreg8(REG_SSTAT2));
            rc = 1;
            goto fail;
        }
        count++;
    }

fail:
    /* Disable interrupts */
    set_ncrreg8(REG_DIEN, 0);
    CachePostDMA((APTR) script, &script_len, DMA_ReadFromRAM);
    if (rc != 0)
        printf("\n");
    return (rc);
}

static void
print_bits(bitdesc_t *bits, uint value)
{
    uint bit;
    for (bit = 0; value != 0; value >>= 1, bit++) {
        if (value & 1) {
            printf(" %s", bits[bit]);
        }
    }
}

static void
print_bits_dash(bitdesc_t *bits, uint value)
{
    uint lastbit = (uint) -1;
    uint bit;
    uint printed = 0;
    for (bit = 0; value != 0; value >>= 1, bit++) {
        if (value & 1) {
            if (printed == 0) {
                printed = 1;
                printf(" %s", bits[bit]);
                lastbit = bit;
            }
        } else if (printed) {
            printed = 0;
            if (lastbit != (bit - 1))
                printf("-%s", bits[bit - 1]);
        }
    }
    if (printed) {
        if (lastbit != (bit - 1))
            printf("-%s", bits[bit - 1]);
    }
}

static int
decode_registers(void)
{
    const char *fmt;
    int         reg;
    uint32_t    value;

    printf("  Reg    Value  Name     Description\n");

    for (reg = 0; reg < ARRAY_SIZE(ncr_regdefs); reg++) {
        if (ncr_regdefs[reg].show == 0)
            continue;
        printf("   %02x ", ncr_regdefs[reg].reg_loc);

        if (ncr_regdefs[reg].reg_size == 1) {
            value = get_ncrreg8(ncr_regdefs[reg].reg_loc);
        } else {
            value = get_ncrreg32(ncr_regdefs[reg].reg_loc & ~3);
            value &= (0xffffffff >> ((ncr_regdefs[reg].reg_loc & 3) * 8));
        }
        switch (ncr_regdefs[reg].reg_size) {
            case 1:
                fmt = "      %0*x";
                break;
            case 2:
                fmt = "    %0*x";
                break;
            case 3:
                fmt = "  %0*x";
                break;
            default:
                fmt = "%0*x";
                break;
        }
        printf(fmt, ncr_regdefs[reg].reg_size * 2, value);
        printf("  %-8s %s",
               ncr_regdefs[reg].reg_name, ncr_regdefs[reg].reg_desc);
        if (ncr_regdefs[reg].reg_bits != NULL) {
            print_bits(ncr_regdefs[reg].reg_bits, value);
        }
        printf("\n");
    }
    return (0);
}


#if 0
static int
dma_mem_to_fifo(uint32_t src, uint32_t len)
{
    uint8_t ctest5 = get_ncrreg8(REG_CTEST5);
    uint8_t ctest8 = get_ncrreg8(REG_CTEST8);

    /* Clear DMA and SCSI FIFOs */
    set_ncrreg8(REG_CTEST8, ctest8 | REG_CTEST8_CLF);
    (void) get_ncrreg8(REG_CTEST8);
    set_ncrreg8(REG_CTEST8, ctest8 & ~REG_CTEST8_CLF);

    /* Set DMA direction host->SCSI and disable SCSI request/ack of DMA */
    set_ncrreg8(REG_CTEST5, ctest5 &
                ~(REG_CTEST5_DACK | REG_CTEST5_DREQ | REG_CTEST5_DDIR));

    /* Assign source or destination address */
    set_ncrreg32(REG_DNAD, src);

    /*
     * Set DMA command and byte count
     * DMA command is high byte
     *   01      = Read/Write Instruction Register
     *     110   = Move to SFBR
     *        00 = Immediate data to destination register
     */
    set_ncrreg32(REG_DCMD, 0x40000000 | len);
    // DCMD and DBC (command & byte count)

#if 1
    printf("DBC1=%x", get_ncrreg32(REG_DCMD));
#endif
    /* Force DMA */
    set_ncrreg8(REG_CTEST8, ctest8 | REG_CTEST8_FLF);
    Delay(1);

    set_ncrreg8(REG_CTEST8, ctest8 & ~REG_CTEST8_FLF);
#if 1
    (void) decode_registers();
    printf(" DBC2=%x\n", get_ncrreg32(REG_DCMD));
#endif

    return (0);
}
#endif

#if 0
/*
 * dma_mem_to_scratch
 * ------------------
 * This function does not work. It was an attempt to implement a
 * CPU-configured test where the only fetch from Amiga memory would be
 * due to the DMA. This would have eliminated the possibility that an
 * errant fetch of the next SIOP instruction could cause a bad DMA write to
 * Amiga memory. This could occur if there are floating or bridged address
 * lines between the 53C710 and the 4091 bus tranceivers. Unfortunately,
 * I could not figure out a way to get the 53C710 to execute the command
 * written to the REG_DCMD register.
 */
static int
dma_mem_to_scratch(uint32_t src)
{
    uint8_t ctest5 = get_ncrreg8(REG_CTEST5);
    uint8_t ctest8 = get_ncrreg8(REG_CTEST8);
    uint8_t dcntl  = get_ncrreg8(REG_DCNTL);
    uint8_t dmode  = get_ncrreg8(REG_DMODE);

    set_ncrreg8(REG_DMODE, dmode | REG_DMODE_MAN);
    set_ncrreg8(REG_DCNTL, dcntl | REG_DCNTL_SSM);

    /* Clear DMA and SCSI FIFOs */
    set_ncrreg8(REG_CTEST8, ctest8 | REG_CTEST8_CLF);
    (void) get_ncrreg8(REG_CTEST8);
    set_ncrreg8(REG_CTEST8, ctest8 & ~REG_CTEST8_CLF);

    /* Set DMA direction host->SCSI and disable SCSI request/ack of DMA */
    set_ncrreg8(REG_CTEST5, ctest5 &
                ~(REG_CTEST5_DACK | REG_CTEST5_DREQ | REG_CTEST5_DDIR));

    printf("src=%x [%08x]\n", src, *ADDR32(src));

    /* Assign source address */
    set_ncrreg32(REG_DSPS, src);

    /* Assign destination address */
    set_ncrreg32(REG_TEMP, a4091_reg_base + REG_SCRATCH);

    /*
     * Set DMA command and byte count
     * DMA command is high byte
     *   11               = Memory Move
     *     000000         = Reserved
     *           ... 0100 = Length (4 bytes)
     */
    set_ncrreg32(REG_DCMD, 0xc0000000 | 0x04);

    printf("DBC1=%x", get_ncrreg32(REG_DCMD));

#if 0
    set_ncrreg8(REG_DCNTL, dcntl | REG_DCNTL_SSM | REG_DCNTL_STD);
#endif

    /* Force DMA */
    set_ncrreg8(REG_CTEST8, ctest8 | REG_CTEST8_FLF);
    Delay(1);

    set_ncrreg8(REG_CTEST8, ctest8 & ~REG_CTEST8_FLF);
    set_ncrreg8(REG_DCNTL, dcntl);
    set_ncrreg8(REG_DMODE, dmode);
#if 0
    (void) decode_registers();
    printf(" DBC2=%x\n", get_ncrreg32(REG_DCMD));
#endif

    /*
     * Try to force DMA memory-memory (also doesn't work)
     * -----------------------------------
     * Set up chip post-reset
     *     med cb 44800038 01 ; cb 4480003b e1 ; cb 44800018 80
     * Set up memory
     *     med cl 00780000 12345678 87654abc
     * Set up DMA
     *     med cl 44800030 00780000 ; cl 4480001c 00780004
     * Trigger DMA
     *     med cl 44800024 c0000004
     * Check DMA
     *     med dl 00780000 8
     *
     * Try to trigger increment of DNAD
     *     med cl 40800028 00780000
     *     med cb 40800025 4
     *     med cb 4080001a 80
     *     med dl 40800028 1
     */

    return (0);
}
#endif

/*
 * dma_mem_to_mem
 * --------------
 * Perform a memory-to-memory copy using the 53C710 DMA engine.
 */
static int
dma_mem_to_mem(uint32_t src, uint32_t dst, uint32_t len)
{
    uint32_t *script = dma_mem_move_script;

    script[0] = 0xc0000000 | len;
    script[1] = src;
    script[2] = dst;
//  CACHE_LINE_WRITE(script, 0x10);

    if (runtime_flags & FLAG_DEBUG)
        printf("DMA from %08x to %08x len %08x\n", src, dst, len);

#if 0
    /* Hold destination address constant */
    set_ncrreg8(REG_DMODE, get_ncrreg8(REG_DSTAT) | REG_DMODE_FAM);
#endif

    return (execute_script(script, 0x10));
}

/*
 * dma_mem_to_scratch
 * -------------------
 * Copy 4 bytes of memory to the 53C710 SCRATCH register.
 */
static int
dma_mem_to_scratch(uint32_t src)
{
    uint32_t dst = a4091_reg_base + REG_SCRATCH;
    return (dma_mem_to_mem(src, dst, 4));
}

/*
 * dma_scratch_to_mem
 * -------------------
 * Copy 4 bytes from the 53C710 SCRATCH register to memory.
 */
static int
dma_scratch_to_mem(uint32_t dst)
{
    uint32_t src = a4091_reg_base + REG_SCRATCH;

    return (dma_mem_to_mem(src, dst, 4));
}

/*
 * dma_scratch_to_temp
 * -------------------
 * Copy 4 bytes of memory from the 53C710 SCRATCH to TEMP register.
 */
static int
dma_scratch_to_temp(void)
{
    uint32_t src = a4091_reg_base + REG_SCRATCH;
    uint32_t dst = a4091_reg_base + REG_TEMP;

    return (dma_mem_to_mem(src, dst, 4));
}

/*
 * dma_mem_to_mem_quad
 * -------------------
 * Performs four memory-to-memory copies of the same data for benchmarking
 * purposes.
 */
static int
dma_mem_to_mem_quad(VAPTR src, VAPTR dst, uint32_t len,
                    int update_script)
{
    uint32_t *script = dma_mem_move_script_quad;

    if (update_script) {
        script[0] = 0xc0000000 | len;
        script[1] = (uint32_t) src;
        script[2] = (uint32_t) dst;
        script[3] = 0xc0000000 | len;
        script[4] = (uint32_t) src;
        script[5] = (uint32_t) dst;
        script[6] = 0xc0000000 | len;
        script[7] = (uint32_t) src;
        script[8] = (uint32_t) dst;
        script[9] = 0xc0000000 | len;
        script[10] = (uint32_t) src;
        script[11] = (uint32_t) dst;
//      CACHE_LINE_WRITE(script, 0x30);
    }

    if (runtime_flags & FLAG_DEBUG)
        printf("DMA from %08x to %08x len %08x\n",
               (uint32_t) src, (uint32_t) dst, len);

    return (execute_script(script, 0x20));
}
/*
 * script_write_reg_setup
 * ----------------------
 * Create the SCRIPT program to write an immediate 8-bit value to
 * the specified register.
 */
static void
script_write_reg_setup(uint32_t *script, uint8_t reg, uint8_t value)
{
    /*
     * Command
     *
     * Bits 31-30 01       = IO
     * Bits 29-27 111      = Read-Modify-Write
     * Bits 26-25 00       = Immediate data to destination register
     * Bit  24    0        = Carry Enable
     * Bits 23-22 00       = Reserved
     * Bits 21-16 111000   = A5-A0 = SCRATCH (0x34)
     * Bits 15-8  00000000 = Immediate data
     * Bits 7-0   00000000 = Reserved
     */
    script[0] = 0x78000000 | (reg << 16) | (value << 8);
    script[1] = 0x98080000;
    script[2] = 0x00000000;
    script[3] = 0x00000000;
//  CACHE_LINE_WRITE(script, 0x10);
}

#if 0
/*
 * script_write_reg
 * ----------------
 * Write an immediate 8-bit value to the specified register
 */
static int
script_write_reg(uint32_t *script, uint8_t reg, uint8_t value)
{
    script_write_reg_setup(script, reg, value);

    if (runtime_flags & FLAG_DEBUG)
        printf("Reg %02x = %02x\n", reg, value);

    return (execute_script(script, 0x10));
}
#endif

static void
show_dip(uint8_t switches, int bit)
{
    printf("  SW %d %-3s ", bit + 1, (switches & BIT(bit)) ? "Off" : "On");
}

static int
decode_switches(void)
{
    uint8_t switches;
    uint32_t addr;

    if (runtime_flags & FLAG_IS_A4000T) {
        addr = a4091_switch_base;
        printf("A4000T");
    } else {
        addr = a4091_switch_base;
        printf("A4091");
    }
    switches = *ADDR8(addr);
    printf(" Rear-access DIP switches\n");

    show_dip(switches, 7);
    printf("SCSI LUNs %s\n", (switches & BIT(7)) ? "Enabled" : "Disabled");
    show_dip(switches, 6);
    if (switches & BIT(6))
        printf("External Termination On\n");
    else
        printf("External Termination Off\n");
    show_dip(switches, 5);
    printf("%s SCSI Mode\n",
           (switches & BIT(5)) ? "Synchronous" : "Asynchronous");
    show_dip(switches, 4);
    printf("%s Spinup\n", (switches & BIT(4)) ? "Short" : "Long");
    show_dip(switches, 3);
    printf("SCSI%s Bus Mode\n",
           (switches & BIT(3)) ? "-2 Fast" : "-1 Standard");
    show_dip(switches, 2);
    printf("ADR2=%d\n", !!(switches & 4));
    show_dip(switches, 1);
    printf("ADR1=%d\n", !!(switches & 2));
    show_dip(switches, 0);
    printf("ADR0=%d  Controller Host ID=%x\n", switches & 1, switches & 7);

    return (0);
}

/*
 * Test overview
 * -------------
 * Device Access
 * Register Access
 * SCSI FIFO
 * DMA FIFO
 * DMA operations
 * DMA copy
 * DMA copy benchmark
 * Interrupts
 * Loopback
 * SCRIPTS Processor
 *
 * =========================================================================
 *
 * Device Access
 * -------------
 * Check for bus timeout
 * Autoconfig area at address
 * Registers found at address
 *
 *
 * Register Access
 * ---------------
 * Registers have sane values
 * Reset chip via registers
 * Verify default (cleared) state of status registers
 * Walking bits test of two writable registers (SCRATCH and TEMP)
 *
 *
 * DMA FIFO Test
 * -------------
 * Write data into DMA FIFO and verify it is retrieved in the same order.
 *
 * Test the basic ability to write data into the DMA FIFO and retrieve
 * it in the same order as written. The DMA FIFO is checked for an empty
 * condition following a software reset, then the FBL2 bit is set and
 * verified. The FIFO is then filled with 16 bytes of data in the four
 * byte lanes verifying the byte lane full or empty with each write. Next
 * the FIFO is read verifying the data and the byte lane full or empty
 * with each read. If no errors are detected then the NCR device is reset,
 * otherwise the device is left in the test state.
 *
 *
 * SCSI FIFO test
 * --------------
 * Write data into SCSI FIFO and verify it is retrieved in the same order.
 *
 * Tests the basic ability to write data into the SCSI FIFO and retrieve
 * it in the same order as written. The SCSI FIFO is checked for an
 * empty condition following a software reset, then the SFWR bit is set
 * and verified. The FIFO is then filled with 8 bytes of data verifying
 * the byte count with each write.  Next the SFWR bit is cleared and the
 * FIFO read verifying the byte count with each read. If no errors are
 * detected then the NCR device is reset, otherwise the device is left
 * in the test state.
 *
 *
 * DMA operations
 * --------------
 * Test DMA from/to host memory.
 *
 *
 * Interrupt test
 * --------------
 * Verifies that level 0 interrupts will not generate an interrupt,
 * but will set the appropriate status. The test then verifies that all
 * interrupts (1-7) can be generated and received and that the appropriate
 * status is set.
 *
 *
 * Loopback test
 * -------------
 * The 53C710 Loopback Mode in effect, lets the chip talk to itself. When
 * the Loopback Enable (SLBE) bit is set in the CTEST4 register, the
 * 53C710 allows control of all SCSI signals. The test checks the Input
 * and Output Data Latches and performs a selection, with the 53C710
 * executing initiator instructions and the host CPU implementing the
 * target role by asserting and polling the appropriate SCSI signals.
 * If no errors are detected then the NCR device is reset, otherwise the
 * device is left in the test state.
 *
 *
 * SCSI SCRIPTS processor possible tests
 * -------------------------------------
 * SCSI Interrupt Enable
 * DMA Interrupt Enable
 * SCSI Status Zero
 * DMA Status
 * Interrupt Status
 * SCSI First Byte Received
 *
 * Set SCSI outputs in high impedance state, disable interrupts using
 * the "MIEN", and set NCR device for Single Step Mode.
 *
 * The address of a simple "INTERRUPT instruction" SCRIPT is loaded
 * into the DMA SCRIPTS Pointer register. The SCRIPTS processor is
 * started by hitting the "STD" bit in the DMA Control Register.
 *
 * Single Step is checked by verifying that ONLY the first instruction
 * executed and that the correct status bits are set. Single Step Mode
 * is then turned off and the SCRIPTS processor started again. The
 * "INTERRUPT instruction" should then be executed and a check for the
 * correct status bits set is made.
 *
 * The address of the "JUMP instruction" SCRIPT is loaded into the DMA
 * SCRIPTS Pointer register, and the SCRIPTS processor is automatically
 * started.  JUMP "if TRUE" (Compare = True, Compare = False) conditions
 * are checked, then JUMP "if FALSE" (Compare = True, Compare = False)
 * conditions are checked.
 */

static int
check_ncrreg_bits(int mode, uint reg, const char *regname, uint8_t rbits)
{
    uint8_t regval = get_ncrreg8(reg);

    if (regval & rbits) {
        const char *modestr = (mode == 0) ? "reserved" : "unexpected";
        printf("%s reg %02x [value %02x] has %s bits set: %02x\n",
               regname, reg, regval, modestr, regval & rbits);
        return (1);
    }
    return (0);
}

static void
show_test_state(const char * const name, int state)
{
    if (state == 0) {
        if (flag_verbose)
            printf("PASS\n");
        return;
    }

    if (flag_verbose || (state != -1))
        printf("  %-16s ", name);

    if (state == -1) {
        fflush(stdout);
        return;
    }
    printf("FAIL\n");
}

static const uint8_t zorro_expected_cregs[] = {
    0x6f, 0x54, 0x30, 0x00, 0x02, 0x02
};

static const char * const zorro_expected_cregnames[] = {
    "Zorro Size / Autoboot",
    "Product",
    "Devvtype/shutup/sizeext",
    "Mfg High",
    "Mfg Low",
    "Serial 0",
    "Serial 1",
    "Serial 2",
    "Serial 3",
    "ROM Hi",
    "ROM Lo",
};

static const uint8_t rom_expected_data[] =
{
    0x9f, 0xaf, 0xcf, 0xff, 0xff, 0xff, 0xff, 0xfd,
    0xff, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,

    0x0f, 0xbf, 0xff, 0xff, 0xdf, 0xdf, 0xff, 0xd5,
    0xff, 0xe6, 0xdf, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,

    0x10, 0x00,
};

/*
 * test_device_access
 * ------------------
 * A4091 device is verified for basic access.
 *
 * 1. Check for bus timeout to ROM area
 * 2. Check for bus timeout to 53C710 registers
 * 3. Verify autoconfig area header contents
 */
static int
test_device_access(void)
{
    uint8_t  saw_incorrect[ARRAY_SIZE(rom_expected_data)];
    int      i;
    int      rc = 0;
    int      pass;
    uint64_t tick_start;
    uint8_t  pins_stuck_high = 0xff;
    uint8_t  pins_stuck_low  = 0xff;
    uint8_t  pins_diff       = 0x00;

    show_test_state("Device access:", -1);

    /* Measure access speed against possible bus timeout */
    tick_start = read_system_ticks();
    (void) *ADDR32(a4091_rom_base);
    if (access_timeout("ROM base timeout", 2, tick_start)) {
        /* Try again */
        (void) *ADDR32(a4091_rom_base);
        if (access_timeout("ROM base timeout", 2, tick_start)) {
            rc = 1;
            goto fail;
        }
    }

    (void) *ADDR32(a4091_reg_base);
    if (access_timeout("\n53C710 access timeout", 2, tick_start)) {
        rc = 1;
        goto fail;
    }

    if (runtime_flags & FLAG_IS_A4000T)
        goto fail;  // Skip autoconfig test

    /* Verify ROM area header contents (overlaps with autoconfig test below) */
    memset(saw_incorrect, 0, sizeof (saw_incorrect));
    for (pass = 0; pass < 50; pass++) {
        for (i = 0; i < ARRAY_SIZE(rom_expected_data); i++) {
            uint8_t val;
            uint8_t diff;

            if ((i & 0x7) == 0) {
                /* Only occasionally measure access time */
                tick_start = read_system_ticks();
                val = get_rom(i);
                if (access_timeout("\nROM access timeout", 2, tick_start)) {
                    rc = 1;
                    printf("    ROM pos %02x at pass %u\n", i, pass);
                    goto fail;
                }
            } else {
                val = get_rom(i);
            }
            pins_stuck_high &= val;
            pins_stuck_low  &= ~val;
            diff = val ^ rom_expected_data[i];
            if (diff != 0) {
                pins_diff |= diff;
                if (saw_incorrect[i] == 0) {
                    saw_incorrect[i] = 1;
                    if (rc == 0)
                        printf("\n");
                    printf("    ROM pos %02x: %02x != expected %02x "
                           "(diff %02x) at pass %u\n",
                           i, val, rom_expected_data[i], diff, pass);
                    rc++;
                }
            }
        }
    }

    if (rc != 0) {
        pins_diff &= ~(pins_stuck_high | pins_stuck_low);
        if (pins_stuck_high != 0) {
            printf("Stuck high: 0x%02x (check for short to VCC)\n",
                   pins_stuck_high);
        }
        if (pins_stuck_low != 0) {
            printf("Stuck low: 0x%02x (check for short to GND)\n",
                   pins_stuck_low);
        }
        if (pins_diff != 0) {
            printf("Floating or bridged: 0x%02x\n", pins_diff);
        }
        goto fail;
    }

    /* Verify autoconfig area header contents */
    memset(saw_incorrect, 0, sizeof (saw_incorrect));
    for (pass = 0; pass < 50; pass++) {
        for (i = 0; i < ARRAY_SIZE(zorro_expected_cregs); i++) {
            uint8_t regval;
            if ((i & 0x7) == 0) {
                /* Only occasionally measure access time */
                tick_start = read_system_ticks();
                regval = get_creg(i);
                if (access_timeout("\nCReg access timeout", 1, tick_start)) {
                    rc = 1;
                    goto fail;
                }
            } else {
                regval = get_creg(i);
            }
            if (regval != zorro_expected_cregs[i]) {
                if (saw_incorrect[i] == 0) {
                    saw_incorrect[i] = 1;
                    if (rc == 0)
                        printf("\n");
                    printf("    Reg %02x: %02x != expected %02x (diff %02x) "
                           "for %s at P%u\n",
                           i, regval, zorro_expected_cregs[i],
                           regval ^ zorro_expected_cregs[i],
                           zorro_expected_cregnames[i], pass);
                    rc++;
                }
            }
        }
    }

fail:
    show_test_state("Device access:", rc);
    return (rc);
}

static int
is_running_in_uae(void)
{
    struct IntVector *iv    = &SysBase->IntVects[A4091_IRQ];
    struct List      *slist = iv->iv_Data;
    struct Interrupt *s;

    if (EMPTY(slist))
        return (0);

    for (s = FIRST(slist); NEXTINT(s); s = NEXTINT(s)) {
        const char *name = GetNodeName((struct Node *) s);
        if (strncmp(name, "UAE", 3) == 0)
            return (1);
    }
    return (0);
}

static bitdesc_t data_pins[] = {
    "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7",
    "D8", "D9", "D10", "D11", "D12", "D13", "D14", "D15",
    "D16", "D17", "D18", "D19", "D20", "D21", "D22", "D23",
    "D24", "D25", "D26", "D27", "D28", "D29", "D30", "D31",
};

static bitdesc_t addr_pins[] = {
    "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7",
    "A8", "A9", "A10", "A11", "A12", "A13", "A14", "A15",
    "A16", "A17", "A18", "A19", "A20", "A21", "A22", "A23",
    "A24", "A25", "A26", "A27", "A28", "A29", "A30", "A31",
};

/*
 * test_register_access
 * --------------------
 * 1. Verify 53C710 reserved register bits are as expected
 * 2. Reset chip via registers
 * 3. Verify default (cleared) state of status registers
 * 4. Walking bits test of writable registers
 */
static int
test_register_access(void)
{
    int pass;
    int rc = 0;
    uint32_t stuck_high;
    uint32_t stuck_low;
    uint32_t pins_diff;
    uint32_t patt;
    uint32_t next;
    uint32_t value;
    uint     rot;
    uint     pos;
    uint     mode;

    show_test_state("Register test:", -1);

    /* Verify reserved bits are as expected */
    for (pass = 0; pass < 100; pass++) {
        rc += check_ncrreg_bits(0, REG_SCNTL1, "SCNTL1", BIT(1) | BIT(0));
        rc += check_ncrreg_bits(0, REG_DSTAT, "DSTAT", BIT(6));
        rc += check_ncrreg_bits(0, REG_CTEST0, "CTEST0", BIT(7) | BIT(1));
        rc += check_ncrreg_bits(0, REG_CTEST0, "CTEST2", BIT(7));
        rc += check_ncrreg_bits(0, REG_ISTAT, "ISTAT", BIT(4) | BIT(2));
        rc += check_ncrreg_bits(0, REG_DIEN, "DIEN", BIT(7) | BIT(6));

        if (rc != 0)
            break;
    }

    a4091_reset();
    dma_clear_istat();

    /* Verify status registers have been cleared */
    rc += check_ncrreg_bits(1, REG_ISTAT, "ISTAT", 0xff);
    rc += check_ncrreg_bits(1, REG_DSTAT, "DSTAT", 0x7f);

    /* Walking bits test of writable registers (TEMP and SCRATCH) */
    patt       = 0xf0e7c3a5;
    stuck_high = 0xffffffff;
    stuck_low  = 0xffffffff;
    pins_diff  = 0x00000000;
    for (mode = 0; mode < 4; mode++) {
        /*
         * mode 0: 32-bit write and 32-bit read
         * mode 1:  8-bit write and 32-bit read
         * mode 2: 32-bit write and  8-bit read
         * mode 3:  8-bit write and  8-bit read
         */
        for (rot = 0; rot < 256; rot++, patt = next) {
            uint32_t got_scratch;
            uint32_t got_temp;
            uint32_t diff_s;
            uint32_t diff_t;
            next = (patt << 1) | (patt >> 31);
            if ((mode & BIT(0)) == 0) {
                /* mode=0 and mode=2 */
                set_ncrreg32(REG_SCRATCH, patt);
                set_ncrreg32(REG_TEMP, next);
            } else {
                /* mode=1 and mode=3 */
                set_ncrreg32b(REG_SCRATCH, patt);
                set_ncrreg32b(REG_TEMP, next);
            }

            if ((mode & BIT(1)) == 0) {
                /* mode=0 and mode=1 */
                got_scratch = get_ncrreg32(REG_SCRATCH);
                got_temp    = get_ncrreg32(REG_TEMP);
            } else {
                /* mode=2 and mode=3 */
                got_scratch = get_ncrreg32b(REG_SCRATCH);
                got_temp    = get_ncrreg32b(REG_TEMP);
            }
            stuck_high &= (got_scratch & got_temp);
            stuck_low  &= ~(got_scratch | got_temp);
            diff_s = got_scratch ^ patt;
            diff_t = got_temp    ^ next;
            if (diff_s != 0) {
                pins_diff |= diff_s;
                if (rc++ == 0)
                    printf("\n");
                if (rc < 8) {
                    printf("Reg SCRATCH %08x != %08x (diff %08x) W%u R%u\n",
                           got_scratch, patt, diff_s,
                           (mode & BIT(0)) ? 8 : 32,
                           (mode & BIT(1)) ? 8 : 32);
                }
            }
            if (diff_t != 0) {
                pins_diff |= diff_t;
                if (rc++ == 0)
                    printf("\n");
                if (rc < 8) {
                    printf("Reg TEMP    %08x != %08x (diff %08x) W%u R%u\n",
                           got_temp, next, diff_t,
                           (mode & BIT(0)) ? 8 : 32,
                           (mode & BIT(1)) ? 8 : 32);
                }
            }
            /* Change pattern */
            if ((rot == 127) || (rot == 255))
                next = ~next;
        }
    }
    if (rc >= 8)
        printf("...\n");
    pins_diff &= ~(stuck_high | stuck_low);
    if (stuck_high != 0) {
        printf("Stuck high: %08x", stuck_high);
        print_bits_dash(data_pins, stuck_high);
        printf(" (check for short to VCC)\n");
    }
    if (stuck_low != 0) {
        printf("Stuck low: %08x", stuck_low);
        print_bits_dash(data_pins, stuck_low);
        printf(" (check for short to GND)\n");
    }
    if (pins_diff != 0) {
        printf("Floating or bridged: %08x", pins_diff);
        print_bits_dash(data_pins, pins_diff);
        printf("\n");
    }
    if (rc != 0)
        goto register_test_end;

    /*
     * Test byte access to scratch register (exercise A0/A1)
     */
    patt = 0x01020304;
    set_ncrreg32(REG_SCRATCH, patt);
    for (pos = 0; pos < 4; pos++) {
        uint8_t data = get_ncrreg8(REG_SCRATCH + pos);
        uint8_t expected = patt >> (8 * (3 - pos));
        if (data != expected) {
            printf("Byte read SCRATCH %08x value %02x != expected %02x\n",
                   patt, data, expected);
            rc++;
        }
    }
    if (rc != 0) {
        value = get_ncrreg32(REG_SCRATCH);
        if (value != patt)
            printf("    also SCRATCH %08x != expected %08x\n", value, patt);
    }
    patt = 0x04030201;
#if 0
    for (pos = 0; pos < 4; pos++)
        set_ncrreg8(REG_SCRATCH + pos, patt >> (8 * (3 - pos)));
#else
    set_ncrreg32b(REG_SCRATCH, patt);
#endif
    value = get_ncrreg32(REG_SCRATCH);
    if (value != patt) {
        printf("Byte writes to SCRATCH %08x != expected %08x\n", value, patt);
        rc++;
    }
    if (rc != 0)
        goto register_test_end;

    /* Test chip increment address and decrement byte count operations */
    set_ncrreg32(REG_DCMD, 0x00000010);  // Set REG_DBC to 0x000010
    set_ncrreg32(REG_DNAD, 0x00000000);  // Clear address
    set_ncrreg8(REG_CTEST5, REG_CTEST5_ADCK);
    value = get_ncrreg32(REG_DNAD);
    if (value != 0x00000004) {
        rc++;
        printf("\nDNAD address increment failed: 0x%x != 0x4\n", value);
    }
    value = get_ncrreg32(REG_DCMD);
    if (value != 0x10) {
        if (rc++ == 0)
            printf("\n");
        printf("DBC (byte count) affected: 0x%x != 0x0\n", value);
    }
    value = get_ncrreg8(REG_CTEST5);
    if (value & REG_CTEST5_ADCK) {
        if (rc++ == 0)
            printf("\n");
        printf("ADCK did not automatically clear in CTEST5\n");
    }

    set_ncrreg8(REG_CTEST5, REG_CTEST5_BBCK);
    value = get_ncrreg32(REG_DCMD);
    if (value != 0x0000000c) {
        rc++;
        printf("\nDBC (byte count) decrement failed: 0x%x != 0xc\n", value);
    }

    value = get_ncrreg8(REG_CTEST5);
    if (value & REG_CTEST5_BBCK) {
        if (rc++ == 0)
            printf("\n");
        printf("BBCK did not automatically clear in CTEST5\n");
    }

register_test_end:
    show_test_state("Register test:", rc);
    return (rc);
}


/*
 * rand32
 * ------
 * Very simple pseudo-random number generator
 */
static uint32_t rand_seed = 0;
static uint32_t
rand32(void)
{
    rand_seed = (rand_seed * 25173) + 13849;
    return (rand_seed);
}

/*
 * srand32
 * -------
 * Very simple random number seed
 */
static void
srand32(uint32_t seed)
{
    rand_seed = seed;
}


/*
 * test_dma_fifo
 * -------------
 * This test writes data into DMA FIFO and then verifies that it has been
 * retrieved in the same order. FIFO full/empty status is checked along
 * the way.
 *
 * 1. Reset the 53C710 and verify the DMA FIFO is empty.
 * 2. Set FBL bits in CTEST4 while filling each byte lane with 16 bytes,
 *    verifying each FIFO is full (64 bytes total stored).
 * 3. Unload the DMA FIFO, verifying all stuffed data values and that
 *    FIFO status is reported as expected.
 */
static int
test_dma_fifo(void)
{
    int     rc = 0;
    int     lane;
    uint8_t ctest1;
    uint8_t ctest4;
    uint8_t ctest7;
    int     cbyte;

    /* The DMA FIFO test fails in FS-UAE due to incomplete emulation */
    if (is_running_in_uae())
        return (0);

    show_test_state("DMA FIFO test:", -1);

    a4091_reset();

    ctest1 = get_ncrreg8(REG_CTEST1);
    if (ctest1 != 0xf0) {
        printf("DMA FIFO not empty before test: "
               "CTEST1 should be 0xf0, but is 0x%02x\n", ctest1);
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            return (0x0f);
    }

    /* Verify FIFO is empty */
    if ((get_ncrreg8(REG_DSTAT) & REG_DSTAT_DFE) == 0) {
        if (rc++ == 0)
            printf("\n");
        printf("DMA FIFO not empty: DSTAT DFE not 1\n");
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            return (0x0f);
    }

    ctest4 = get_ncrreg8(REG_CTEST4);
    ctest7 = get_ncrreg8(REG_CTEST7) & ~BIT(3);

    /* Push bytes to all byte lanes of DMA FIFO */
    srand32(19700119);
    for (lane = 0; lane < 4; lane++) {
        /* Select byte lane */
        set_ncrreg8(REG_CTEST4, (ctest4 & ~3) | REG_CTEST4_FBL2 | lane);

        /* Push bytes to byte lane of DMA FIFO, including parity */
        for (cbyte = 0; cbyte < NCR_FIFO_SIZE; cbyte++) {
            uint16_t rvalue = rand32() >> 8;
            uint8_t  pvalue = ctest7 | ((rvalue >> 5) & BIT(3));
            // XXX: Verify FIFO is not full
            set_ncrreg8(REG_CTEST7, pvalue);
            set_ncrreg8(REG_CTEST6, rvalue);
            if (runtime_flags & FLAG_DEBUG)
                printf(" %02x", rvalue & 0x1ff);
        }
    }

    /* Verify FIFO is not empty */
    if (get_ncrreg8(REG_DSTAT) & REG_DSTAT_DFE) {
        if (rc++ == 0)
            printf("\n");
        printf("DMA FIFO is empty: DSTAT DFE not 1\n");
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            return (0x0f);
    }

    ctest1 = get_ncrreg8(REG_CTEST1);
    if (ctest1 != 0x0f) {
        printf("DMA FIFO not full: CTEST1 should be 0x0f, but is 0x%02x\n",
               ctest1);
        rc = 0xff;
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            goto fail;
    }

    /* Pop bytes from byte lanes of DMA FIFO */
    srand32(19700119);
    for (lane = 0; lane < 4; lane++) {
        int count = 0;
        set_ncrreg8(REG_CTEST4, (ctest4 & ~3) | REG_CTEST4_FBL2 | lane);

        /* Pop bytes from byte lane of DMA FIFO, attaching parity as bit 8 */
        for (cbyte = 0; cbyte < NCR_FIFO_SIZE; cbyte++) {
            uint16_t rvalue = (rand32() >> 8) & (BIT(9) - 1);
            uint16_t value  = get_ncrreg8(REG_CTEST6);
            uint16_t pvalue = (get_ncrreg8(REG_CTEST2) & BIT(3)) << 5;
            value |= pvalue;
            if (value != rvalue) {
                if (((rc & BIT(lane)) == 0) || (count++ < 2)) {
                    if (rc == 0)
                        printf("\n");
                    printf("Lane %d byte %d FIFO got %03x, expected %03x\n",
                           lane, cbyte, value, rvalue);
                } else if (count == 3) {
                    printf("...\n");
                }
                rc |= BIT(lane);
            }
        }
    }

    ctest1 = get_ncrreg8(REG_CTEST1);
    if (ctest1 != 0xf0) {
        printf("\nDMA FIFO not empty after test: "
               "CTEST1 should be 0xf0, but is 0x%02x\n", ctest1);
        rc = 0xff;
    }

fail:
    /* Restore normal operation */
    set_ncrreg8(REG_CTEST4, ctest4 & ~7);

    show_test_state("DMA FIFO test:", rc);
    return (rc);
}

/*
 * test_scsi_fifo
 * --------------
 * This test writes data into SCSI FIFO and then verifies that it has been
 * retrieved in the same order. FIFO full/empty status is checked along
 * the way.
 *
 * 1. Reset the 53C710 and verify the DMA FIFO is empty.
 * 2. Set SCSI FIFO Write Enable bit in CTEST4.
 * 3. The data is loaded into the SCSI FIFO by writing to the SODL register.
 * 4. FIFO data parity can be written in one of two ways:
 *    A. Parity can flow into the SIOP on the parity signals if the Enable
 *       Parity Generation bit in the SCNTLO register equals O. The PU
 *       drives the parity signal for the corresponding 8-bit data signals.
 *    B. If the Parity Generation bit is equal to 1, then the SIOP forces
 *       the parity bit to even or odd parity. Set the Assert Even SCSI Parity
 *       bit in the SCNTL1 register to 0 to load the SCSI FIFO with odd parity.
 *       If this bit is equal to 1, then the SCSI FIFO will be loaded with even
 *       parity.
 * 5. Read CTEST3 to pull data from the SCSI FIFO. CTEST2 Bit 4 (SCSI FIFO
 *    parity) provides FIFO parity after reading CTEST3.
 * 6. Unload the SCSI FIFO, verifying all stuffed data values and that
 *    FIFO status is reported as expected.
 */
static int
test_scsi_fifo(void)
{
    int     rc = 0;
    int     lane;
    uint8_t ctest1;
    uint8_t ctest4;
    uint8_t ctest7;
    int     cbyte;

#if 0
    /* The SCSI FIFO test fails in FS-UAE due to incomplete emulation */
    if (is_running_in_uae())
        return (0);
#endif

    show_test_state("SCSI FIFO test:", -1);

    // XXX: The below code has not been changed yet from the DMA FIFO test
    a4091_reset();

    ctest1 = get_ncrreg8(REG_CTEST1);
    if (ctest1 != 0xf0) {
        printf("SCSI FIFO not empty before test: "
               "CTEST1 should be 0xf0, but is 0x%02x\n", ctest1);
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            return (0x0f);
    }

    /* Verify FIFO is empty */
    if ((get_ncrreg8(REG_DSTAT) & REG_DSTAT_DFE) == 0) {
        if (rc++ == 0)
            printf("\n");
        printf("SCSI FIFO not empty: DSTAT DFE not 1\n");
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            return (0x0f);
    }

    ctest4 = get_ncrreg8(REG_CTEST4);
    ctest7 = get_ncrreg8(REG_CTEST7) & ~BIT(3);

    /* Push bytes to all byte lanes of SCSI FIFO */
    srand32(19700119);
    for (lane = 0; lane < 4; lane++) {
        /* Select byte lane */
        set_ncrreg8(REG_CTEST4, (ctest4 & ~3) | REG_CTEST4_FBL2 | lane);

        /* Push bytes to byte lane of SCSI FIFO, including parity */
        for (cbyte = 0; cbyte < NCR_FIFO_SIZE; cbyte++) {
            uint16_t rvalue = rand32() >> 8;
            uint8_t  pvalue = ctest7 | ((rvalue >> 5) & BIT(3));
            // XXX: Verify FIFO is not full
            set_ncrreg8(REG_CTEST7, pvalue);
            set_ncrreg8(REG_CTEST6, rvalue);
            if (runtime_flags & FLAG_DEBUG)
                printf(" %02x", rvalue);
        }
    }

    /* Verify FIFO is not empty */
    if (get_ncrreg8(REG_DSTAT) & REG_DSTAT_DFE) {
        if (rc++ == 0)
            printf("\n");
        printf("SCSI FIFO is empty: DSTAT DFE not 1\n");
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            return (0x0f);
    }

    ctest1 = get_ncrreg8(REG_CTEST1);
    if (ctest1 != 0x0f) {
        printf("SCSI FIFO not full: CTEST1 should be 0x0f, but is 0x%02x\n",
               ctest1);
        rc = 0xff;
        if ((runtime_flags & FLAG_MORE_DEBUG) == 0)
            goto fail;
    }

    /* Pop bytes from byte lanes of SCSI FIFO */
    srand32(19700119);
    for (lane = 0; lane < 4; lane++) {
        int count = 0;
        set_ncrreg8(REG_CTEST4, (ctest4 & ~3) | REG_CTEST4_FBL2 | lane);

        /* Pop bytes from byte lane of SCSI FIFO, attaching parity as bit 8 */
        for (cbyte = 0; cbyte < NCR_FIFO_SIZE; cbyte++) {
            uint16_t rvalue = (rand32() >> 8) & (BIT(9) - 1);
            uint16_t value  = get_ncrreg8(REG_CTEST6);
            uint16_t pvalue = (get_ncrreg8(REG_CTEST2) & BIT(3)) << 5;
            value |= pvalue;
            if (value != rvalue) {
                if (((rc & BIT(lane)) == 0) || (count++ < 2)) {
                    if (rc == 0)
                        printf("\n");
                    printf("Lane %d byte %d FIFO got %02x, expected %02x\n",
                           lane, cbyte, value, rvalue);
                } else if (count == 3) {
                    printf("...\n");
                }
                rc |= BIT(lane);
            }
        }
    }

    ctest1 = get_ncrreg8(REG_CTEST1);
    if (ctest1 != 0xf0) {
        printf("\nSCSI FIFO not empty after test: "
               "CTEST1 should be 0xf0, but is 0x%02x\n", ctest1);
        rc = 0xff;
    }

fail:
    /* Restore normal operation */
    set_ncrreg8(REG_CTEST4, ctest4 & ~7);

    show_test_state("SCSI FIFO test:", rc);
    return (rc);
}

/*
 * test_loopback
 * -------------
 * This test writes data into DMA FIFO and then verifies that it has been
 * retrieved in the same order. FIFO full/empty status is checked along
 * the way.
 *
 * 1. Put the 53C710 in loopback mode (this lets the host directly modify
 *    SCSI bus signals).
 * 2. Trigger a selection, with the 53C710 acting as SCSI initiator
 * 3. The host will act as a fake target device.
 * 4. Verify correct selection sequence takes place.
 * 5. Reset the 53C710.
 *
 * From the 53C700 Data manual:
 * How to Test the SIOP in the Loopback Mode
 * -----------------------------------------
 * SIOP loopback mode allows testing of both initiator and target operations.
 * When the Loopback Enable bit is 1 in the CTEST4 register, the SIOP allows
 * control of all SCSI signals, whether the SIOP is operating in initiator
 * or target mode. Perform the following steps to implement loopback mode:
 : 1. Set the Loopback Enable bit in the CTEST4 register to 1.
 * 2. Set-up the desired arbitration mode as defined in the SCNTLO register.
 * 3. Set the Start Sequence bit in the SCNTLO register to 1.
 * 4. Poll the SBCL register to determine when SEU is active and BSYI is
 *    inactive.
 * 5. Poll the SBDL register to determine which SCSI ID bits are being driven.
 * 6. In response to selection, set the BSYI bit (bit 5) in the SOCL register
 *    to 1.
 * 7. Poll the SEU bit in the SBCL register to determine when SEU becomes
 *    inactive.
 * 8. To assert the desired phase, set the MSG/, CID, and I/O bits to the
 *    desired phase in the SOCL register.
 * 9. To assert REQI, keep the phase bits the same and set the REQI bit in
 *    the SOCL register to 1. To accommodate the 400 ns bus settle delay,
 *    set REQI after setting the phase signals,
 * 10. The initiator role can be implemented by single stepping SCSI SCRIPTS
 *     and the SIOP can loopback as a target or vice versa.
 */
#if 0
static int
test_loopback(void)
{
    int     rc = 0;
    int     pass;
    int     bit;
    uint8_t ctest4;

    show_test_state("SIOP loopback test:", -1);

    ctest4 = get_ncrreg8(REG_CTEST4);

    a4091_reset();
    set_ncrreg8(REG_CTEST4, ctest4 | REG_CTEST4_SLBE);




    set_ncrreg8(REG_CTEST4, ctest4);
    a4091_reset();

    show_test_state("SIOP loopback test:", rc);
    return (rc);
}
#endif

static bitdesc_t scsi_data_pins[] = {
    "SCDAT0", "SCDAT1", "SCDAT2", "SCDAT3",
    "SCDAT4", "SCDAT5", "SCDAT6", "SCDAT7",
    "SCDATP",
};
static bitdesc_t scsi_control_pins[] = {
    "SCTRL_IO", "SCTRL_CD", "SCTRL_MSG", "SCTRL_ATN",
    "SCTRL_SEL", "SCTRL_BSY", "SCTRL_ACK", "SCTRL_REQ",
};

static uint
calc_parity(uint8_t data)
{
    data ^= (data >> 4);
    data ^= (data >> 2);
    data ^= (data >> 1);
    return (!(data & 1));
}

/*
 * test_scsi_pins
 * --------------
 * This test uses override bits in the CTEST registers to allow the CPU
 * to set and clear SCSI data and control pin signals. With that access,
 * pins are checked to verify they can be set and cleared, and that those
 * operations do not affect the state of other SCSI pins.
 */
static int
test_scsi_pins(void)
{
    int     rc = 0;
    int     pass;
    int     bit;
    uint8_t ctest4;
    uint8_t dcntl;
    uint8_t scntl0;
    uint8_t scntl1;
    uint8_t sstat1;
    uint8_t sbcl;
    uint8_t sbdl;
    uint    stuck_high;
    uint    stuck_low;
    uint    pins_diff;

    show_test_state("SCSI pin test:", -1);

    ctest4 = get_ncrreg8(REG_CTEST4);
    scntl0 = get_ncrreg8(REG_SCNTL0);
    scntl1 = get_ncrreg8(REG_SCNTL1);
    dcntl  = get_ncrreg8(REG_DCNTL);

    a4091_reset();
    Delay(1);

    /* Check that SCSI termination power is working */
    sbdl = get_ncrreg8(REG_SBDL);
    sbcl = get_ncrreg8(REG_SBCL);
    sbcl |= 0x20; // Not sure why, but STRCL_BSY might still be high
    if ((sbcl == 0xff) && (sbdl == 0xff)) {
        if (rc++ == 0)
            printf("FAIL\n");
        printf("\tAll SCSI pins low: check term power D309A and F309A/F309B\n");
        return (rc);
    }

    /* Check that bus is not stuck in reset */
    sstat1 = get_ncrreg8(REG_SSTAT1);
    if (sstat1 & REG_SSTAT1_RST) {
        if (rc++ == 0)
            printf("FAIL\n");
        printf("\tSCSI bus is in reset: check for SCTRL_RST short to GND\n");
        return (rc);
    }

    /* Test reset */
    set_ncrreg8(REG_SCNTL1, REG_SCNTL1_RST);
    Delay(1);
    sstat1 = get_ncrreg8(REG_SSTAT1);
    if ((sstat1 & REG_SSTAT1_RST) == 0) {
        if (rc++ == 0)
            printf("FAIL\n");
        printf("\tSCSI bus could not be reset: "
               "check for SCTRL_RST short to VCC\n");
    }
    set_ncrreg8(REG_SCNTL1, 0);
    Delay(1);

    /* Check that bus is not stuck busy */
    sbcl = get_ncrreg8(REG_SBCL);
    if (sbcl & 0x20) {
        if (rc++ == 0)
            printf("FAIL\n");
        printf("\tSCSI bus is stuck busy: check for SCTRL_BSY short to GND\n");
    }


    /* Set registers to manually drive SCSI data and control pins */
    set_ncrreg8(REG_DCNTL,  dcntl | REG_DCNTL_LLM);
    set_ncrreg8(REG_CTEST4, ctest4 | REG_CTEST4_SLBE);
    set_ncrreg8(REG_SCNTL0, REG_SCNTL0_EPG);
    set_ncrreg8(REG_SCNTL1, REG_SCNTL1_ADB);

    /* Walk a test pattern on SODL and verify that it arrives on SBDL */
    set_ncrreg8(REG_SOCL, 0x00);
    stuck_high = 0x1ff;
    stuck_low  = 0x1ff;
    pins_diff  = 0x000;
    for (pass = 0; pass < 2; pass++) {
        for (bit = -1; bit < 8; bit++) {
            uint    din;
            uint    dout = BIT(bit);
            uint    diff;
            uint8_t parity_exp;
            uint8_t parity_got;

            /*
             * Pass 0 = Walking ones
             * Pass 1 = Walking zeros
             */
            if (pass == 1)
                dout = (uint8_t) ~dout;

            set_ncrreg8(REG_SODL, dout);
            din = get_ncrreg8(REG_SBDL);
            parity_got = get_ncrreg8(REG_SSTAT1) & REG_SSTAT1_PAR;
            parity_exp = calc_parity(dout);
            dout |= (parity_exp << 8);
            din  |= (parity_got << 8);
            stuck_high &= din;
            stuck_low  &= ~din;
            diff = din ^ dout;

            if ((diff & 0xff) != 0)
                diff &= 0xff;  // Ignore parity when other bits differ
            if (diff != 0) {
                pins_diff |= diff;
                if (rc++ == 0)
                    printf("FAIL\n");
                if (rc <= 8) {
                    printf("\tSCSI data %03x != expected %03x [diff %03x]",
                           din, dout, diff);
                    print_bits(scsi_data_pins, diff);
                    printf("\n");
                }
            }
        }
    }
    /* Note: Register state is inverted from SCSI pin state */
    pins_diff &= ~(stuck_high | stuck_low);
    if (stuck_high != 0) {
        printf("Stuck low [%02x]", stuck_high);
        print_bits(scsi_data_pins, stuck_high);
        printf(": check for short to GND\n");
    }
    if (stuck_low != 0) {
        printf("Stuck high [%02x]", stuck_low);
        print_bits(scsi_data_pins, stuck_low);
        printf(": check for short to VCC\n");
    }
    if (pins_diff != 0) {
        printf("Floating or bridged [%02x]", pins_diff);
        print_bits(scsi_data_pins, pins_diff);
        printf("\n");
    }

    set_ncrreg8(REG_SODL, 0xff);

    /* Walk a test pattern on SOCL and verify that it arrives on SBCL */
    stuck_high = 0xff;
    stuck_low  = 0xff;
    pins_diff  = 0x00;
    for (pass = 0; pass < 2; pass++) {
        for (bit = -1; bit < 8; bit++) {
            uint8_t din;
            uint8_t dout = BIT(bit);
            uint8_t diff;

            /*
             * Pass 0 = Walking ones
             * Pass 1 = Walking zeros
             */
            if (pass == 1)
                dout = (uint8_t) ~dout;

            /*
             * Eliminate testing certain combinations
             * Never assert bit 3 (SCTRL_SEL)
             */
            if ((dout == 0x80) || (dout == 0x40) || (dout == 0xf7) ||
                (dout & BIT(3)))
                continue;

            set_ncrreg8(REG_SOCL, dout);
            din = get_ncrreg8(REG_SBCL);
            stuck_high &= din;
            stuck_low  &= ~din;
            diff = din ^ dout;
            if (diff != 0) {
                pins_diff |= diff;
                if (rc++ == 0)
                    printf("\n");
                if (rc <= 8) {
                    printf("SCSI control %02x != expected %02x (diff %02x",
                           din, dout, diff);
                    print_bits(scsi_control_pins, diff);
                    printf(")\n");
                }
            }
        }
    }

    /* Note: Register state is inverted from SCSI pin state */
    stuck_low  &= ~(BIT(3) | BIT(6) | BIT(7));
    pins_diff  &= ~(stuck_high | stuck_low);

    if (stuck_high != 0) {
        printf("Stuck low: %02x", stuck_high);
        print_bits(scsi_control_pins, stuck_high);
        printf(" (check for short to GND)\n");
    }
    if (stuck_low != 0) {
        printf("Stuck high: %02x", stuck_low);
        print_bits(scsi_control_pins, stuck_low);
        printf(" (check for short to VCC)\n");
    }
    if (pins_diff != 0) {
        printf("Floating or bridged: %02x", pins_diff);
        print_bits(scsi_control_pins, pins_diff);
        printf("\n");
    }

    set_ncrreg8(REG_DCNTL,  dcntl);
    set_ncrreg8(REG_SCNTL0, scntl0);
    set_ncrreg8(REG_SCNTL1, scntl1);
    set_ncrreg8(REG_CTEST4, ctest4);
    a4091_reset();

    show_test_state("SCSI pin test:", rc);
    return (rc);
}

/*
 * AllocMem_aligned
 * ----------------
 * Allocate CPU memory with the specified minimum alignment.
 */
static APTR
AllocMem_aligned(uint len, uint alignment)
{
    APTR addr;
    Forbid();
    addr = AllocMem(len + alignment, MEMF_PUBLIC);
    if (addr != NULL) {
        FreeMem(addr, len + alignment);
        addr = (APTR) AllocAbs(len, (APTR) (((uint32_t) addr + alignment - 1) &
                                            ~(alignment - 1)));
    }
    Permit();
    return (addr);
}

/*
 * mem_not_zero
 * ------------
 * Return a non-zero value if the specified memory block is not all zero.`
 */
static int
mem_not_zero(uint32_t paddr, uint len)
{
    uint32_t *addr = (uint32_t *)paddr;
    len >>= 2;
    while (len-- > 0)
        if (*(addr++) != 0)
            return (1);
    return (0);
}

/*
 * alloc_power2_addr
 * -----------------
 * Search for memory which can be allocated at a specific power-of-two
 * address. Two memory addresses are returned. One is a memory address
 * without that address bit set, and the alternate is a memory address
 * with that address bit set.
 *
 * If both memory addresses can not be allocated, NULL will be
 * returned for both.
 */
static uint32_t *
alloc_power2_addr(uint bit, uint32_t **ret_addr1, uint alloc_addr1_only)
{
    uint      bit1;
    uint      bit2;
    uint32_t  taddr0;
    uint32_t  taddr1;
    uint32_t  base  = 0;          // Chipmem base
    uint32_t  top   = (2 << 20);  // 2 MB chipmem
    uint32_t *addr0 = NULL;
    uint32_t *addr1 = NULL;
    struct ExecBase  *eb = SysBase;
    struct MemHeader *mem;

    /* Walk memory list for base addresses to try */
    for (mem = (struct MemHeader *)eb->MemList.lh_Head;
         mem->mh_Node.ln_Succ != NULL;
         mem = (struct MemHeader *)mem->mh_Node.ln_Succ) {
        for (bit2 = 1; bit2 < 32; bit2++) {
            if (BIT(bit2) >= top)
                break;
            for (bit1 = bit2; bit1 < 32; bit1++) {
                if (BIT(bit1) >= top)
                    break;
                if ((bit1 == bit) || (bit2 == bit) || (bit1 == bit2))
                    continue;
                taddr0 = (base | BIT(bit1) | BIT(bit2)) & ~BIT(1);
                taddr1 = (base | BIT(bit1) | BIT(bit2) | BIT(bit)) & ~BIT(1);
                addr1 = AllocAbs(0x10, (void *) taddr1);
                if (addr1 == NULL)
                    continue;
                if (alloc_addr1_only == 0) {
                    addr0 = AllocAbs(0x10, (void *) taddr0);
                    if (addr0 == NULL) {
                        FreeMem(addr1, 0x10);
                        continue;
                    }
                }
                if (runtime_flags & FLAG_DEBUG)
                    printf("%2u,%2u,%2u %08x %08x\n", bit, bit1, bit2,
                           (uint32_t) addr0, (uint32_t) addr1);

                /* Found pair of memory chunks */
                *ret_addr1 = addr1;
                return (addr0);
            }
        }
        base = (uint32_t) mem;
        top  = (uint32_t) mem->mh_Upper;
    }
    *ret_addr1 = NULL;
    return (NULL);
}

/*
 * is_valid_ram
 * ------------
 * Returns non-zero when the specified address is in a system memory
 * block. The address is valid regardless of whether the RAM is currently
 * free or allocated.
 */
int
is_valid_ram(uint32_t addr)
{
    struct ExecBase  *eb = SysBase;
    struct MemHeader *mem;

    for (mem = (struct MemHeader *)eb->MemList.lh_Head;
         mem->mh_Node.ln_Succ != NULL;
         mem = (struct MemHeader *)mem->mh_Node.ln_Succ) {
        uint32_t base = (uint32_t) mem;
        uint32_t top  = (uint32_t) mem->mh_Upper;
        if ((addr >= base) && (addr < top))
            return (1);
    }
    return (0);
}

#if 0
static void
nonfunctional_tests(void)
{
    /*
     * Attempt to inject a command at REG_DSPS which will get fetched
     * and executed as a SCRIPTS command. DOES NOT WORK.
     */
    uint32_t *addr = ADDR32((a4091_reg_base + REG_DSPS);
    addr[0] = 0x98080000;
    addr[1] = 0x00000000;
//  set_ncrreg32(REG_SCRATCH, 0x00000000);
    CACHE_LINE_WRITE(addr, 0x8);


    /*
     * Attempt to execute a manual command which writes 0xa5 to SCRATCH[0].
     * DOES NOT WORK.
     */
//  set_ncrreg32(REG_DMODE, get_ncrreg32(REG_DMODE) | REG_DMODE_MAN);
    set_ncrreg32(REG_DCNTL, get_ncrreg32(REG_DCNTL) | REG_DCNTL_LLM);
    set_ncrreg32(REG_DCMD, 0x7834a500);
    set_ncrreg32(REG_DSPS, 0);
    set_ncrreg32(REG_DCNTL, get_ncrreg32(REG_DCNTL) | REG_DCNTL_STD);
    decode_registers();
    dma_clear_istat();
}
#endif

/*
 * test_bus_access
 * ---------------
 * Verify that 53C710 can access the host bus by fetching scripts.
 */
static int
test_bus_access(void)
{
    int       rc = 0;
    int       rc2;
    uint      bit;
    uint8_t   got0;
    uint8_t   got1;
    uint32_t *saddr0;
    uint32_t *saddr1;
    uint32_t  pins_diff   = 0;
    uint32_t  stuck_high  = 0xffffffff;
    uint32_t  stuck_low   = 0xffffffff;
    uint32_t  tested_high = 0;
    uint32_t  tested_low  = 0;
    uint32_t  start_intcount;
    uint64_t  tick_start;

    show_test_state("Bus access test:", -1);

    a4091_reset();
    dma_clear_istat();

    /* Verify interrupts can be triggered by the 53C710 */
    start_intcount = a4091_save.intcount;
    tick_start = read_system_ticks();

    set_ncrreg8(REG_DIEN, REG_DIEN_ABRT);  // Set DMA interrupt enable on Abort
    set_ncrreg8(REG_ISTAT, REG_ISTAT_ABRT);
    while (a4091_save.intcount == start_intcount) {
        if (access_timeout("Could not trigger interrupt", 2, tick_start)) {
            printf("ISTAT=%02x %02x DSTAT=%02x %02x SSTAT0=%02x "
                   "SSTAT1=%02x SSTAT2=%02x\n",
                   a4091_save.ireg_istat, get_ncrreg8(REG_ISTAT),
                   a4091_save.ireg_dstat, get_ncrreg8(REG_DSTAT),
                   get_ncrreg8(REG_SSTAT0), get_ncrreg8(REG_SSTAT1),
                   get_ncrreg8(REG_SSTAT2));
            show_test_state("Bus access test:", 1);
            return (1);
        }
    }
    if ((a4091_save.ireg_istat & REG_ISTAT_ABRT) == 0) {
        printf("Interrupt triggered, but abort status not captured\n");
        show_test_state("Bus access test:", 1);
        return (1);
    }

    /*
     * Execute a script at the base address of the ROM. The values here
     * on the A4091 are actually the Zorro config area, where the first
     * value is 0x90f8ff00. This is a conditional RETURN command.
     * So long as the fetch works, this script should just return success.
     *
     * 0x90f8ff00:
     *    1001 0000 1111 1000 1111 1111 0000 0000
     *    10       = Transfer control
     *    010      = RETURN (conditional)
     *    0        = MSG
     *    0        = C/D
     *    0        = I/O
     *    1        = Relative Addressing mode
     *    1        = Reserved (0)
     *    1        = Reserved (0)
     *    1        = Reserved (0)
     *    1        = Jump if True
     *    0        = Compare Data
     *    0        = Compare Phase
     *    0        = Wait for valid phase
     *    11111111 = Mask to compare
     *    00000000 = Data to be compared
     */
    rc = execute_script((uint32_t *) a4091_rom_base, 0x10);
    if (rc != 0) {
        printf("SCRIPTS fetch from A4091 ROM %08x failed\n", a4091_rom_base);
    }

    /* Try to execute a script from each power-of-2 address */
    for (bit = 3; bit < 32; bit++) {
        saddr0 = alloc_power2_addr(bit, &saddr1, FALSE);

        if (saddr0 == NULL)  // Failed, try again with address bit=1 only
            saddr0 = alloc_power2_addr(bit, &saddr1, TRUE);
        if (saddr1 == NULL) {
            /* Could not even allocate address where address bit is high */
            if (saddr0 != NULL)
                FreeMem(saddr0, 0x10);
            continue;
        }

        a4091_reset();
        dma_clear_istat();
        script_write_reg_setup(saddr1, REG_SCRATCH, 0x5a);

        if (saddr0 != NULL) {
            script_write_reg_setup(saddr0, REG_SCRATCH, 0xa5);
            set_ncrreg32(REG_SCRATCH, 0xff);
            if ((rc2 = execute_script(saddr0, 0x10)) != 0) {
                rc++;
#ifdef ABORT_BUS_TEST_ON_FIRST_FAILURE
                FreeMem(saddr0, 0x10);
                FreeMem(saddr1, 0x10);
                break;
#endif
            }
            got0 = get_ncrreg32(REG_SCRATCH);  // 0xa5 expected
            if (got0 == 0xa5) {
                /* Success: address under test where bit = 0 */
                stuck_high &= ~BIT(bit);
                tested_low |= BIT(bit);
            } else if (got0 == 0x5a) {
                /* Fail: address under test where bit = 0, got bit = 1 addr */
                pins_diff |= BIT(bit);
                tested_low |= BIT(bit);
            } else {
                /* address line */
                if (rc++ == 0)
                    printf("\n");
                printf("A%u=%u bus fetch from %08x failed (%02x)\n",
                       bit, 0, (uint32_t) saddr0, got0);
            }
        }

        set_ncrreg32(REG_SCRATCH, 0xff);
        if ((rc2 = execute_script(saddr1, 0x10)) != 0) {
            rc++;
#ifdef ABORT_BUS_TEST_ON_FIRST_FAILURE
            if (saddr0 != NULL)
                FreeMem(saddr0, 0x10);
            FreeMem(saddr1, 0x10);
            break;
#endif
        }
        got1 = get_ncrreg32(REG_SCRATCH);  // 0x5a expected

        if (got1 == 0x5a) {
            /* Success: address under test where bit = 1 */
            stuck_low &= ~BIT(bit);
            tested_high |= BIT(bit);
        } else if (got1 == 0xa5) {
            /* Fail: address under test where bit = 1, got bit = 0 addr */
            pins_diff |= BIT(bit);
            tested_high |= BIT(bit);
        } else {
            if (rc++ == 0)
                printf("\n");
            printf("A%u=%u bus fetch from %08x failed (%02x)\n",
                   bit, 1, (uint32_t) saddr1, got1);
        }

        if (saddr0 != NULL)
            FreeMem(saddr0, 0x10);
        FreeMem(saddr1, 0x10);
    }

    if (rc == 0) {
        if (flag_verbose) {
            printf("PASS:");
            print_bits_dash(addr_pins, tested_high | tested_low);
            printf(" tested\n");
        }
    } else {
        show_test_state("Bus access test:", rc);
    }

    pins_diff  &= ~(stuck_high | stuck_low);
    stuck_high &= tested_low;
    stuck_low  &= tested_high;
    if (stuck_high != 0) {
        printf("Stuck high: %08x", stuck_high);
        print_bits_dash(addr_pins, stuck_high);
        printf(" (check for short to VCC)\n");
    }
    if (stuck_low != 0) {
        printf("Stuck low: %08x", stuck_low);
        print_bits_dash(addr_pins, stuck_low);
        printf(" (check for short to GND)\n");
    }
    if (pins_diff != 0) {
        printf("Floating or bridged: %08x", pins_diff);
        print_bits_dash(addr_pins, pins_diff);
        printf("\n");
    }

    return (rc);
}

/*
 * test_dma
 * --------
 * This function will repeatedly perform many DMA operations as reads
 * from CPU memory and writes directly to the 53C710's SCRATCH register.
 * Data from each DMA operation is verified by the CPU reading the
 * SCRATCH register. The source address in CPU memory is incremented for
 * each read. This allows a crude memory address test to be implemented,
 * at least for the address bits A10-A0.
 *
 * Could also implement a SCRIPTS function to move from a memory location
 * into the SCRATCH register. I don't like this so much because the
 * SCRIPTS instruction would need to be fetched from RAM. Could mitigate
 * somewhat by configuring single step mode (DCNTL.bit4).
 */
static int
test_dma(void)
{
    int      rc = 0;
    int      rc2 = 0;
    int      pos;
    uint     dma_len = 2048;
    APTR    *tsrc;
    APTR    *src;
    ULONG    buf_handled;
    uint32_t diff;
    uint32_t addr;
    uint32_t scratch;
    uint32_t temp;

    srand32(time(NULL));
    show_test_state("DMA test:", -1);

    tsrc = AllocMem_aligned(dma_len * 3, dma_len);
    if (tsrc == NULL) {
        printf("Failed to allocate src buffer\n");
        rc = 1;
        goto fail_src_alloc;
    }
    src = tsrc + dma_len;

    a4091_reset();

    buf_handled = 4;

    /* DMA test 1: transfer from 53C710 SCRATCH to TEMP register */
    for (pos = 0; pos < 4; pos++) {
        uint32_t wdata = rand32();
        set_ncrreg32(REG_SCRATCH, wdata);
        set_ncrreg32(REG_TEMP, ~wdata);
        CachePreDMA((APTR) (a4091_reg_base + REG_SCRATCH),
                    &buf_handled, DMA_ReadFromRAM);
        CachePreDMA((APTR) (a4091_reg_base + REG_TEMP),
                    &buf_handled, 0);
        rc = dma_scratch_to_temp();
        CachePostDMA((APTR) (a4091_reg_base + REG_TEMP),
                     &buf_handled, 0);
        CachePostDMA((APTR) (a4091_reg_base + REG_SCRATCH),
                     &buf_handled, DMA_ReadFromRAM);

        if (rc != 0) {
            printf("DMA failed at pos %x for %s\n", pos, "SCRATCH->TEMP");
            scratch = get_ncrreg32(REG_SCRATCH);
            temp    = get_ncrreg32(REG_TEMP);
            diff    = wdata ^ temp;
            printf("  SCRATCH %08x to TEMP %08x: %08x %s= expected %08x\n",
                   a4091_reg_base + REG_SCRATCH,
                   a4091_reg_base + REG_TEMP,
                   temp, (diff != 0) ? "!" : "", wdata);
            if (scratch != wdata) {
                printf("  SCRATCH %08x: %08x != written %08x\n",
                       a4091_reg_base + REG_TEMP, scratch, wdata);
            }
            if ((temp != wdata) && (temp != ~wdata)) {
                printf("  TEMP value %08x != written %08x or expected %08x\n",
                       temp, ~wdata, wdata);
            }
            goto fail_dma;
        }

        temp = get_ncrreg32(REG_TEMP);
        diff = wdata ^ temp;
        if ((diff != 0) && (rc2++ < 10)) {
            /*
             * This test is not aborted on data mismatch errors, so that
             * multiple errors may be captured and reported.
             */
            scratch = get_ncrreg32(REG_SCRATCH);
            if (rc2 == 1)
                printf("\n");
            if (scratch != wdata) {
                printf("  SCRATCH %08x: %08x != written %08x\n",
                       a4091_reg_base + REG_TEMP, scratch, wdata);
            } else {
                printf("  SCRATCH %08x to TEMP: %08x != expected %08x "
                       "(diff %08x)\n",
                       a4091_reg_base + REG_SCRATCH, temp, wdata, diff);
            }
        }
    }
    rc += rc2;
    if (rc != 0)
        goto fail_dma;

    /* DMA test 2: transfer from RAM to the SCRATCH register */
    for (pos = 0; pos < dma_len; pos += 4) {
        uint32_t wdata = rand32();
        addr = (uint32_t) src + pos;
        *ADDR32(addr) = wdata;
        set_ncrreg32(REG_SCRATCH, ~wdata);
        buf_handled = 4;
        CachePreDMA((APTR) addr, &buf_handled, DMA_ReadFromRAM);
        CachePreDMA((APTR) (a4091_reg_base + REG_SCRATCH), &buf_handled, 0);
        rc = dma_mem_to_scratch(addr);
        CachePostDMA((APTR) (a4091_reg_base + REG_SCRATCH), &buf_handled, 0);
        CachePostDMA((APTR) addr, &buf_handled, DMA_ReadFromRAM);

        if (rc != 0) {
            printf("DMA failed at pos %x for %s\n", pos, "RAM->SCRATCH");
            scratch = get_ncrreg32(REG_SCRATCH);
            diff    = *ADDR32(addr) ^ scratch;
            printf("  Addr %08x to SCRATCH %08x: %08x %s= expected %08x\n",
                   addr, a4091_reg_base + REG_SCRATCH,
                   scratch, (diff != 0) ? "!" : "", *ADDR32(addr));
            goto fail_dma;
        }

        scratch = get_ncrreg32(REG_SCRATCH);
        diff = *ADDR32(addr) ^ scratch;
        if (diff != 0) {
            /*
             * This test is not aborted on data mismatch errors, so that
             * multiple errors may be captured and reported.
             */
            if (rc2++ < 8) {
                if (rc2 == 1)
                    printf("\n");
                printf("  Addr %08x to SCRATCH %08x: %08x != expected %08x "
                       "(diff %08x)\n",
                       addr, a4091_reg_base + REG_SCRATCH,
                       scratch, *ADDR32(addr), diff);
                if (wdata != *ADDR32(addr)) {
                    printf("    Source data changed from %08x to %08x\n",
                           wdata, *ADDR32(addr));
                }
            }
        }
    }
    rc += rc2;
    if (rc != 0)
        goto fail_dma;

    /* DMA test 3: transfer from SCRATCH register to RAM */
    for (pos = 0; pos < dma_len; pos += 4) {
        uint32_t wdata = rand32();
        addr = (uint32_t) src + pos;
        *ADDR32(addr) = ~wdata;
        set_ncrreg32(REG_SCRATCH, wdata);
        CachePreDMA((APTR) (a4091_reg_base + REG_SCRATCH),
                    &buf_handled, DMA_ReadFromRAM);
        CachePreDMA((APTR) addr, &buf_handled, 0);
        rc = dma_scratch_to_mem(addr);
        CachePostDMA((APTR) addr, &buf_handled, 0);
        CachePostDMA((APTR) (a4091_reg_base + REG_SCRATCH),
                     &buf_handled, DMA_ReadFromRAM);

        if (rc != 0) {
            printf("DMA failed at pos %x for %s\n", pos, "SCRATCH->RAM");
            scratch = get_ncrreg32(REG_SCRATCH);
            diff    = *ADDR32(addr) ^ scratch;
            printf("  SCRATCH %08x to Addr %08x: %08x %s= expected %08x\n",
                   a4091_reg_base + REG_SCRATCH, addr,
                   *ADDR32(addr), (diff != 0) ? "!" : "", scratch);
            goto fail_dma;
        }

        temp = *ADDR32(addr);
        diff = temp ^ wdata;
        if (diff != 0) {
            /*
             * This test is not aborted on data mismatch errors, so that
             * multiple errors may be captured and reported.
             */
            if (rc2++ < 10) {
                if (rc2 == 1)
                    printf("\n");
                printf("  SCRATCH %08x to addr %08x: %08x != expected %08x "
                       "(diff %08x)\n",
                       a4091_reg_base + REG_SCRATCH,
                       addr, temp, wdata, diff);
            }
        }
    }
    rc += rc2;

fail_dma:
    FreeMem(tsrc, dma_len * 3);
fail_src_alloc:

    show_test_state("DMA test:", rc);
    return (rc);
}

/*
 * test_dma_copy
 * -------------
 * This function will drive a DMA copy from/to Amiga main memory. Since we
 * can't yet trust DMA as working reliably (the controller might have
 * floating address lines), the code tries to protect Amiga memory at bit
 * flip addresses. The destination address is used to determine the
 * addresses which need to be protected. Single and double addr flip addresses
 * are either allocated or copied / restored.
 *
 * I think only 4-byte alignment is required by the 53C710 DMA controller,
 * but 16-byte alignment might be best for burst optimization. Need to test
 * this.
 *
 * XXX: CTEST8.3 FLF (Flush DMA FIFO) may be used to write data in DMA FIFO
 *      to address in DNAD.
 *      CTEST5.DMAWR controls direction of the transfer.
 */
static int
test_dma_copy(void)
{
#define DMA_LEN_BIT 12 // 4K DMA
    int       rc = 0;
    uint      dma_len = BIT(DMA_LEN_BIT);
    uint      cur_dma_len = 4;
    uint      pass;
    uint      pos;
    uint      bit1;
    uint      bit2;
    uint      bf_mismatches = 0;
    uint      bf_copies     = 0;
    uint      bf_buffers    = 0;
    uint      bf_notram     = 0;
    VAPTR    *src_backup;
    APTR     *src;
    APTR     *dst;
    APTR     *dst_buf;
    uint32_t *bf_addr;
    uint32_t *bf_mem;
    uint8_t   bf_flags[32][32];
    ULONG     buf_handled;
#define BF_FLAG_COPY    0x01
#define BF_FLAG_CORRUPT 0x02
#define BF_FLAG_NOTRAM  0x04
#define BF_ADDR(x, y) bf_addr[(x) * 32 + y]
#define BF_MEM(x, y)  bf_mem[(x) * 32 + y]

    show_test_state("DMA copy:", -1);

    srand32(time(NULL));
    memset(bf_flags, 0, sizeof (bf_flags));

    src_backup = AllocMem_aligned(dma_len, 16);
    if (src_backup == NULL) {
        printf("Failed to allocate src_backup buffer\n");
        rc = 1;
        goto fail_src_backup_alloc;
    }
    src = AllocMem_aligned(dma_len, 16);
    if (src == NULL) {
        printf("Failed to allocate src buffer\n");
        rc = 1;
        goto fail_src_alloc;
    }
    dst_buf = AllocMem_aligned(dma_len * 3, 16);
    if (dst_buf == NULL) {
        printf("Failed to allocate dst buffer\n");
        rc = 1;
        goto fail_dst_alloc;
    }

    /* Land DMA in the middle of the buffer */
    dst = (APTR) ((uint32_t) dst_buf + dma_len);

#define BFADDR_SIZE (32 * 32 * sizeof (uint32_t))
    bf_addr = AllocMem(BFADDR_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    bf_mem = AllocMem(BFADDR_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if ((bf_addr == NULL) || (bf_mem == NULL)) {
        printf("Failed to allocate protection array\n");
        rc = 1;
        goto fail_bfaddr_alloc;
    }

    for (bit1 = DMA_LEN_BIT; bit1 < 32; bit1++) {
        for (bit2 = bit1; bit2 < 32; bit2++) {
            if (bit1 == bit2)
                BF_ADDR(bit1, bit2) = ((uint32_t) dst) ^ BIT(bit1);
            else
                BF_ADDR(bit1, bit2) = ((uint32_t) dst) ^ BIT(bit1) ^ BIT(bit2);
            BF_MEM(bit1, bit2) =
                (uint32_t) AllocAbs(dma_len, (APTR) BF_ADDR(bit1, bit2));
            if (BF_MEM(bit1, bit2) != 0) {
                /* Got target memory -- pattern it */
                uint pos;
                uint32_t *ptr;
got_target_mem:
                ptr = (uint32_t *) BF_MEM(bit1, bit2);
                for (pos = 0; pos < dma_len; pos += 4)
                    *(ptr++) = BF_MEM(bit1, bit2) + pos;
                bf_buffers++;
            } else if (src == (APTR) BF_ADDR(bit1, bit2)) {
                /* Special case -- src buffer is address we want */
                BF_MEM(bit1, bit2) = (uint32_t) src;
                src = AllocMem_aligned(dma_len, 16);
                if (src == NULL) {
                    src = (APTR) BF_MEM(bit1, bit2);
                    goto fallback_copy_mem;
                }
                goto got_target_mem;
            } else if (src_backup == (APTR) BF_ADDR(bit1, bit2)) {
                /* Special case -- src_backup buffer is address we want */
                BF_MEM(bit1, bit2) = (uint32_t) src_backup;
                src_backup = AllocMem_aligned(dma_len, 16);
                if (src_backup == NULL) {
                    src_backup = (APTR) BF_MEM(bit1, bit2);
                    goto fallback_copy_mem;
                }
                goto got_target_mem;
            } else if (dst == (APTR) BF_ADDR(bit1, bit2)) {
                /* Special case -- dst buffer is address we want */
                BF_MEM(bit1, bit2) = (uint32_t) dst;
                dst = AllocMem_aligned(dma_len, 16);
                if (dst == NULL) {
                    dst = (APTR) BF_MEM(bit1, bit2);
                    goto fallback_copy_mem;
                }
                goto got_target_mem;
            } else if (is_valid_ram(BF_ADDR(bit1, bit2))) {
fallback_copy_mem:
                BF_MEM(bit1, bit2) = (uint32_t) AllocMem(dma_len, MEMF_PUBLIC);
                bf_flags[bit1][bit2] |= BF_FLAG_COPY;
                bf_copies++;
            } else {
                bf_flags[bit1][bit2] |= BF_FLAG_NOTRAM;
                bf_notram++;
            }
        }
    }

    if (runtime_flags & FLAG_DEBUG)
        printf("\nDMA src=%08x srcb=%08x dst=%08x len=%x\n",
               (uint32_t) src, (uint32_t) src_backup, (uint32_t) dst, dma_len);

    if (runtime_flags & FLAG_DEBUG) {
        printf("Watched addresses:\n");
        for (bit1 = DMA_LEN_BIT; bit1 < 32; bit1++) {
            for (bit2 = bit1; bit2 < 32; bit2++) {
                char *state;
                if (bf_flags[bit1][bit2] & BF_FLAG_COPY)
                    state = "?";
                else if (bf_flags[bit1][bit2] & BF_FLAG_NOTRAM)
                    state = "?";
                else
                    state = "";
                printf(" %s%08x", state, BF_ADDR(bit1, bit2));
            }
            printf("\n");
        }
    }

    a4091_reset();
    dma_clear_istat();
    Forbid();
    for (bit1 = DMA_LEN_BIT; bit1 < 32; bit1++) {
        for (bit2 = bit1; bit2 < 32; bit2++) {
            if (bf_flags[bit1][bit2] & BF_FLAG_COPY) {
                memcpy((APTR)BF_MEM(bit1, bit2),
                       (APTR)BF_ADDR(bit1, bit2), dma_len);
            }
        }
    }
    for (pass = 0; pass < 32; pass++) {
        for (pos = 0; pos < cur_dma_len; pos += 4) {
            uint32_t rvalue = rand32();
            *ADDR32((uint32_t) src + pos) = rvalue;
            *ADDR32((uint32_t) src_backup + pos) = rvalue;
            *ADDR32((uint32_t) dst + pos) = ~rvalue;
        }
        buf_handled = cur_dma_len;
        CachePreDMA((APTR) dst, &buf_handled, DMA_ReadFromRAM);
        CachePostDMA((APTR) dst, &buf_handled, DMA_ReadFromRAM);

        CachePreDMA((APTR) src, &buf_handled, DMA_ReadFromRAM);
        CachePreDMA((APTR) dst, &buf_handled, 0);

        rc = dma_mem_to_mem((uint32_t) src, (uint32_t) dst, cur_dma_len);
        CachePostDMA((APTR) dst, &buf_handled, 0);
        CachePostDMA((APTR) src, &buf_handled, DMA_ReadFromRAM);

        if (rc != 0) {
            /* DMA operation failed */
            printf("DMA src=%08x dst=%08x len=%02x failed\n",
                   (uint32_t) src, (uint32_t) dst, cur_dma_len);
        }

        /* Verify data landed where it was expected */
        for (pos = 0; pos < cur_dma_len; pos += 4) {
            uint32_t svalue = *ADDR32((uint32_t) src + pos);
            uint32_t dvalue = *ADDR32((uint32_t) dst + pos);
            if (svalue != dvalue) {
                if (rc == 0) {
                    printf("\nDMA src=%08x dst=%08x len=%x\n",
                           (uint32_t) src, (uint32_t) dst, cur_dma_len);
                }
                if ((rc < 5) || (runtime_flags & FLAG_DEBUG)) {
                    uint32_t sbvalue = *ADDR32((uint32_t) src_backup + pos);
                    printf("Addr %08x value %08x != expected %08x "
                           "(diff %08x)\n",
                           (uint32_t) dst + pos, dvalue, svalue,
                           dvalue ^ svalue);
                    /* Search power-of-two buffers for corruption */
                    for (bit1 = DMA_LEN_BIT; bit1 < 32; bit1++) {
                        for (bit2 = bit1; bit2 < 32; bit2++) {
                            if (bf_flags[bit1][bit2] & BF_FLAG_NOTRAM) {
                                continue;
                            } else if (bf_flags[bit1][bit2] & BF_FLAG_COPY) {
                                uint32_t cpyaddr  = BF_MEM(bit1, bit2) + pos;
                                uint32_t cpyvalue = *(uint32_t *) cpyaddr;
                                uint32_t oaddr    = BF_ADDR(bit1, bit2) + pos;
                                uint32_t ovalue   = *(uint32_t *) oaddr;
                                if (cpyvalue != ovalue) {
                                    printf("  Watched address %08x changed "
                                           "from %08x to %08x\n",
                                           oaddr, cpyvalue, ovalue);
                                }
                            } else {
                                uint32_t chkaddr = BF_MEM(bit1, bit2) + pos;
                                uint32_t chkvalue = *(uint32_t *) chkaddr;
                                if (chkvalue != chkaddr) {
                                    printf("  Watched address %08x corrupt "
                                           "value %08x found\n",
                                           chkaddr, chkvalue);
                                }
                            }
                        }
                    }
                    if (svalue != sbvalue) {
                        printf("    Source data at %08x changed from "
                               "%08x to %08x\n",
                               (uint32_t) src + pos, sbvalue, svalue);
                    }
                }
                rc++;
            }
        }

        /*
         * If any part of the landing area is wrong, look for the missing
         * data elsewhere in memory (address line floating or shorted).
         */
        if (rc > 0) {
            if (rc > 5)
                printf("...");
            printf("%d total miscompares\n", rc);

            /* Miscompare -- attempt to locate the data elsewhere in memory */


            /*
             * For now, this code will just display all address blocks
             * which differ from the before-test version.
             */
            for (bit1 = DMA_LEN_BIT; bit1 < 32; bit1++) {
                for (bit2 = bit1; bit2 < 32; bit2++) {
                    if (bf_flags[bit1][bit2] & BF_FLAG_NOTRAM) {
                        continue;
                    } else if (bf_flags[bit1][bit2] & BF_FLAG_COPY) {
                        if (memcmp((APTR)BF_MEM(bit1, bit2),
                                   (APTR)BF_ADDR(bit1, bit2), dma_len) != 0) {
                            bf_flags[bit1][bit2] |= BF_FLAG_CORRUPT;
                            if (bf_mismatches++ == 0)
                                printf("Modified RAM blocks:");
                            printf(" (%08x)", BF_ADDR(bit1, bit2));
                        }
                    } else if (mem_not_zero(BF_MEM(bit1, bit2), dma_len)) {
                        if (bf_mismatches++ == 0)
                            printf("Modified RAM blocks:");
                        printf(" %08x", BF_ADDR(bit1, bit2));
                    }
                }
            }
            if (bf_mismatches != 0)
                printf("\n");
        }

        if (rc != 0)
            break;

        cur_dma_len <<= 1;
        if (cur_dma_len >= dma_len)
            cur_dma_len = dma_len;
    }
    Permit();

    if (bf_mismatches != 0) {
        printf("  Watched buffers=%u copies=%u notram=%u mismatches=%u\n",
               bf_buffers, bf_copies, bf_notram, bf_mismatches);
    }

    /* Deallocate protected memory */
    for (bit1 = DMA_LEN_BIT; bit1 < 32; bit1++) {
        for (bit2 = bit1; bit2 < 32; bit2++) {
            if (BF_MEM(bit1, bit2) != 0) {
                FreeMem((APTR) BF_MEM(bit1, bit2), dma_len);
            }
        }
    }

fail_bfaddr_alloc:
    if (bf_mem != NULL)
        FreeMem(bf_mem, BFADDR_SIZE);
    if (bf_addr != NULL)
        FreeMem(bf_addr, BFADDR_SIZE);
    FreeMem(dst_buf, dma_len * 3);
fail_dst_alloc:
    FreeMem(src, dma_len);

fail_src_alloc:
    FreeMem((APTR *)src_backup, dma_len);

fail_src_backup_alloc:
    show_test_state("DMA copy:", rc);
    return (rc);
}

/*
 * test_dma_copy_perf
 * ------------------
 * This test benchmarks repeated 64K DMA from and to CPU memory.
 * The expected performance is right around 5.2MB/sec. There is
 * currently no range checking done on the measured performance.
 */
static int
test_dma_copy_perf(void)
{
    uint      dma_len = 128 << 10;  // DMA maximum is 16MB - 1
    int       rc = 0;
    int       pass;
    int       total_passes = 0;
    VAPTR     src;
    VAPTR     dst;
    uint64_t  tick_start;
    uint64_t  tick_end;
    ULONG     buf_handled;

    show_test_state("DMA copy perf:", -1);

    a4091_reset();

    src = AllocMem_aligned(dma_len, 64);
    if (src == NULL) {
        printf("Failed to allocate src buffer\n");
        rc = 1;
        goto fail_src_alloc;
    }
    dst = AllocMem_aligned(dma_len, 64);
    if (dst == NULL) {
        printf("Failed to allocate dst buffer\n");
        rc = 1;
        goto fail_dst_alloc;
    }
    buf_handled = dma_len;
    CachePreDMA((APTR) src, &buf_handled, DMA_ReadFromRAM);
    CachePreDMA(dst, &buf_handled, 0);

    if (runtime_flags & FLAG_DEBUG)
        printf("\nDMA src=%08x dst=%08x len=%x\n",
               (uint32_t) src, (uint32_t) dst, dma_len);

    a4091_reset();
    dma_clear_istat();
    tick_start = read_system_ticks_sync();
run_some_more:
    for (pass = 0; pass < 16; pass++) {
        total_passes++;
        if (dma_mem_to_mem_quad(src, dst, dma_len, pass == 0)) {
            printf("DMA src=%08x dst=%08x len=%02x failed\n",
                   (uint32_t) src, (uint32_t) dst, dma_len);
            rc = 1;
            break;
        }
    }
    if (rc == 0) {
        tick_end = read_system_ticks();
        uint tick_milli = get_milli_ticks(tick_end);
        uint64_t ticks = tick_end - tick_start;
        uint64_t total_kb = total_passes * (dma_len / 1024) * 2 * 4;
        /*                          2=R/w, 4=4 transfers in script */
        const char *passfail = "PASS";
        if (ticks < 10)
            goto run_some_more;

        if (flag_verbose) {
            printf("%s: %u KB in %u.%02u ticks",
                    passfail, (uint32_t) total_kb, (uint32_t) ticks,
                    (tick_milli + 50) / 100);
            if ((ticks + tick_milli) == 0)
                tick_milli = 1;
            printf(" (%u KB/sec)\n",
                   (uint32_t) (total_kb * TICKS_PER_SECOND * 1000 /
                               (ticks * 1000 + tick_milli)));
        }
    }
    CachePostDMA((APTR) src, &buf_handled, DMA_ReadFromRAM);
    CachePostDMA(dst, &buf_handled, 0);

    FreeMem((APTR *) dst, dma_len);
fail_dst_alloc:
    FreeMem((APTR *) src, dma_len);

fail_src_alloc:
    if (rc != 0)
        show_test_state("DMA copy perf:", rc);
    return (rc);
}


static void
show_test_numbers(void)
{
    printf("Numbers may be appended to the -t test command to run a specific "
           "unit test.\n"
           "Example:  a4091 -t56\n"
           "          This will execute both test 5 and test 6.\n"
           "  -0  Device access: Check ROM header and Zorro config area\n"
           "  -1  Register access\n"
           "          53C710 register read-only bits are verified.\n"
           "          TEMP and SCRATCH walking bits test\n"
           "          Byte, word, and long register access (A0 & A1)\n"
           "          53C710 register DNAD/DBC auto increment/decrement\n"
           "  -2  DMA FIFO: 53C710 RAM + parity\n"
           "  -3  SCSI FIFO: 53C710 RAM + parity\n"
           "  -4  Bus access\n"
           "  -5  Simple DMA (4 bytes at a time)\n"
           "          53C710 SCRATCH  -> 53C710 TEMP register\n"
           "          RAM (Fastmem)   -> 53C710 SCRATCH register\n"
           "          53C710 SCRATCH  -> RAM (Fastmem)\n"
           "  -6  Copy block DMA: Main mem->main mem data verify\n"
           "          Note KB/sec is reported but not range checked.\n"
           "  -7  DMA copy performance (no verify)\n"
//         "  -8  SCSI Loopback (not implemented)\n"
           "  -9  SCSI pins: data pins and some control pins\n");
}

/*
 * test_card
 * ---------
 * Tests the specified A4091 card.
 */
static int
test_card(uint test_flags)
{
    int rc = 0;

    if (test_flags == 0)
        test_flags = -1;

    /* Take over 53C710 interrupt handling and register state */
    if ((rc == 0) && (test_flags & BIT(0)))
        rc = test_device_access();

    check_break();
    if ((rc == 0) && (test_flags & BIT(1)))
        rc = test_register_access();

    check_break();
    if ((rc == 0) && (test_flags & BIT(2)))
        rc = test_dma_fifo();

    check_break();
    if ((rc == 0) && (test_flags & BIT(3)))
        rc = test_scsi_fifo();

    check_break();
    if ((rc == 0) && (test_flags & BIT(4)))
        rc = test_bus_access();

    check_break();
    if ((rc == 0) && (test_flags & BIT(5)))
        rc = test_dma();

    check_break();
    if ((rc == 0) && (test_flags & BIT(6)))
        rc = test_dma_copy();

    check_break();
    if ((rc == 0) && (test_flags & BIT(7)))
        rc = test_dma_copy_perf();

#if 0
    /* Loopback test not implemented yet */
    check_break();
    if ((rc == 0) && (test_flags & BIT(8)))
        rc = test_loopback();
#endif

    check_break();
    if ((rc == 0) && (test_flags & BIT(8)))
        rc = test_scsi_pins();

    return (rc);
}

/*
 * a4901_list
 * ----------
 * Display list of all A4091 cards found during autoconfig.
 */
static int
a4091_list(uint32_t addr)
{
    struct Library        *ExpansionBase;
    struct ConfigDev      *cdev = NULL;
    struct CurrentBinding  cbind;
    int                    count = 0;
    int                    did_header = 0;

    if ((ExpansionBase = OpenLibrary(expansion_library_name, 0)) == 0) {
        printf("Could not open %s\n", expansion_library_name);
        return (1);
    }

    do {
        cdev = FindConfigDev(cdev, ZORRO_MFG_COMMODORE, ZORRO_PROD_A4091);
        if (cdev != NULL) {
            count++;
            if (((addr > 0x10) && ((uint32_t) cdev->cd_BoardAddr != addr)) ||
                ((addr > 0) && (addr <= 0x10) && (count != addr))) {
                continue;
            }
            if (did_header == 0) {
                did_header++;
                printf("  Index Address  Size     Flags\n");
            }
            printf("  %-3d   %08x %08"PRIx32,
                   count, (uint32_t) cdev->cd_BoardAddr,
                   cdev->cd_BoardSize);
            if (cdev->cd_Flags & CDF_SHUTUP)
                printf(" ShutUp");
            if (cdev->cd_Flags & CDF_CONFIGME)
                printf(" ConfigMe");
            if (cdev->cd_Flags & CDF_BADMEMORY)
                printf(" BadMemory");
            cbind.cb_ConfigDev = cdev;
            if (GetCurrentBinding(&cbind, sizeof (cbind)) >= sizeof (cbind)) {
                printf(" Bound");
                if (cbind.cb_FileName != NULL)
                    printf(" to %s", cbind.cb_FileName);
                if (cbind.cb_ProductString != NULL)
                    printf(" prod %s", cbind.cb_ProductString);
            }
            printf("\n");
        }
    } while (cdev != NULL);

    if (count == 0)
        printf("No A4091 cards detected\n");
    else if (did_header == 0)
        printf("Specified card %x not detected\n", addr);

    CloseLibrary(ExpansionBase);
    return (count == 0);
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
    uint32_t          addr  = -1;  /* Default to not found */
    int               count = 0;

    if ((ExpansionBase = OpenLibrary(expansion_library_name, 0)) == 0) {
        printf("Could not open %s\n", expansion_library_name);
        return (-1);
    }

    do {
        cdev = FindConfigDev(cdev, ZORRO_MFG_COMMODORE, ZORRO_PROD_A4091);
        if (cdev != NULL) {
            count++;
            if ((pos == 0) || (pos == count)) {
                addr = (uint32_t) cdev->cd_BoardAddr;
                break;
            }
        }
    } while (cdev != NULL);

    CloseLibrary(ExpansionBase);

    return (addr);
}

/*
 * zorro_autoconfig_card
 * ---------------------
 * Attempt to map an A4091 in physical memory, assuming that it failed
 * autoconfig and is still sitting at the Zorro III autoconfig address
 * (0xff000000).
 *
 * 1. If the MMU is active, ask MuTools to allow access to the autoconfig
 *    region:
 *        SYS:MMULib/MuTools/MuSetCacheMode
 *                  address=0xff000000 size=0x01000000 nocache io valid
 * 2. Attempt to detect the card as present. Use DSACK* mode:
 *        med cb de0000 80         // BERR on timeout (slow)
 *        med cb de0000 00         // DSACK* on timeout (fast)
 * 3. Tell the card to map itself at 0x40000000
 *        med cb ff000044 40
 * 4. If the MMU is active, ask MuTools to allow access to the newly
 *    mapped region.
 *        SYS:MMULib/MuTools/MuSetCacheMode
 *                  address=0x40000000 size=0x01000000 nocache io valid
 * 5. Attempt to verify 53C710 registers are present, and maybe that
 *    the DIP switch area repeats every dword.
 */
static void
zorro_autoconfig_card(void)
{

}

/*
 * enforcer_check
 * --------------
 * Verifies enforcer is not running.
 */
static int
enforcer_check(void)
{
    Forbid();
    if (FindTask("« Enforcer »") != NULL) {
        /* Enforcer is running */
        Permit();
        printf("Enforcer is present.  First use \"enforcer off\" to "
               "disable enforcer.\n");
        return (1);
    }
    if (FindTask("« MuForce »") != NULL) {
        /* MuForce is running */
        Permit();
        printf("MuForce is present.  First use \"muforce off\" to "
               "disable MuForce.\n");
        return (1);
    }
    Permit();
    return (0);
}

/*
 * nextarg
 * -------
 * Returns the next argument in the argv list (argument in the list is
 * automatically set to NULL).
 */
static char *
nextarg(int argc, char **argv, int argnum)
{
    for (; argnum < argc; argnum++) {
        char *next = argv[argnum];
        if (next == NULL)
            continue;
        argv[argnum] = NULL;
        return (next);
    }
    return (NULL);
}

/*
 * usage
 * -----
 * Displays program usage.
 */
static void
usage(void)
{
    printf("%s\n\n"
           "This tool will test an installed A4091 SCSI controller for "
           "correct operation.\n"
           "Options:\n"
           "\t-a  specify card address (slot or physical address): <addr>\n"
           "\t-c  decode device autoconfig area\n"
           "\t-d  enable debug output\n"
           "\t-D  perform DMA from/to Amiga memory: <src> <dst> <len>\n"
           "\t-f  ignore fact enforcer is present or driver is loaded\n"
           "\t-h  display this help text\n"
           "\t-k  kill (disable) all active A4091 device drivers\n"
           "\t-L  loop until failure\n"
           "\t-P  probe and list all detected A4091 cards\n"
           "\t-q  quiet mode (only show errors)\n"
           "\t-r  display NCR53C710 registers\n"
           "\t-s  decode device external switches\n"
           "\t-S  attempt to suspend all A4091 drivers while testing\n"
           "\t-t  test card\n"
           "\t-?  show individual test steps\n",
           version + 7);
}

/*
 * main
 * ----
 * Parse and execute arguments.
 */
int
main(int argc, char **argv)
{
    int      arg;
    int      rc             = 0;
    int      flag_config    = 0;  /* Decode device autoconfig area */
    int      flag_dma       = 0;  /* Copy memory using 53C710 DMA engine */
    int      flag_force     = 0;  /* Ignore the fact that enforcer is present */
    int      flag_loop      = 0;  /* Loop all tests until failure */
    int      flag_kill      = 0;  /* Kill active A4091 device driver */
    int      flag_list      = 0;  /* List all A4091 cards found */
    int      flag_regs      = 0;  /* Decode device registers */
    int      flag_switches  = 0;  /* Decode device external switches */
    int      flag_test      = 0;  /* Test card */
    int      flag_suspend   = 0;  /* Suspend A4091 drivers while testing */
    int      flag_zautocfg  = 0;  /* Attempt Zorro Autoconfig */
    uint     test_flags     = 0;  /* Test flags (0-9) */
    uint     pass           = 0;  /* Current test pass */
    uint32_t addr           = 0;  /* Card physical address or index number */
    uint32_t dma[3];              /* DMA source, destination, length */

    __check_abort_enabled = 0;
    memset(&a4091_save, 0, sizeof (a4091_save));

    for (arg = 1; arg < argc; arg++) {
        char *ptr = argv[arg];
        if (ptr == NULL)
            continue;  // Already grabbed this argument
        if (*ptr == '-') {
            while (*(++ptr) != '\0') {
                switch (*ptr) {
                    case '0':
                    case '1':
                    case '2':
                    case '3':
                    case '4':
                    case '5':
                    case '6':
                    case '7':
                    case '8':
                    case '9':
                        test_flags |= BIT(*ptr - '0');
                        break;
                    case 'a': {
                        /* card address */
                        int pos = 0;

                        if (++arg >= argc) {
                            printf("You must specify an address\n");
                            exit(1);
                        }
                        if ((argv[arg][0] == '.') && (argv[arg][1] == '\0')) {
                            addr = A4000T_SCSI_BASE;
                        } else if ((sscanf(argv[arg], "%x%n",
                                           &addr, &pos) != 1) ||
                                   (pos == 0)) {
                            printf("Invalid card address %s specified\n", ptr);
                            exit(1);
                        }
                        if (addr == A4000T_SCSI_BASE + A4000T_OFFSET_REGISTERS)
                            addr = A4000T_SCSI_BASE;
                        break;
                    }
                    case 'c':
                        flag_config = 1;
                        break;
                    case 'd':  /* debug */
                        if (runtime_flags & FLAG_DEBUG)
                            runtime_flags |= FLAG_MORE_DEBUG;
                        else
                            runtime_flags |= FLAG_DEBUG;
                        break;
                    case 'D': {  /* DMA */
                        char *s[3];
                        int  i;
                        int  pos;
                        static const char * const which[] = {
                            "src", "dst", "len"
                        };
                        flag_dma = 1;

                        for (i = 0; i < 3; i++) {
                            s[i] = nextarg(argc, argv, arg + 1);
                            if (s[i] == NULL) {
                                printf("Command requires <src> <dst> <len>\n");
                                exit(1);
                            }
                            if ((sscanf(s[i], "%x%n", &dma[i], &pos) != 1) ||
                                (pos == 0)) {
                                printf("Invalid DMA %s %s specified\n",
                                        which[i], s[i]);
                                exit(1);
                            }
                        }
                        break;
                    }
                    case 'h':
                        usage();
                        exit(0);
                    case 'f':
                        flag_force = 1;
                        break;
                    case 'k':
                        flag_kill = 1;
                        break;
                    // case 'l':  // Reserved for loop count
                    case 'L':
                        flag_loop = 1;
                        break;
                    case 'P':
                        flag_list = 1;
                        break;
                    case 'q':
                        flag_verbose = 0;
                        break;
                    case 'r':
                        flag_regs = 1;
                        break;
                    case 's':
                        flag_switches = 1;
                        break;
                    case 'S':
                        flag_suspend = 1;
                        break;
                    case 't':
                        flag_test++;
                        break;
                    case 'z':
                        flag_zautocfg++;
                        break;
                    case '?':
                        show_test_numbers();
                        exit(1);
                    default:
                        printf("Unknown -%s\n", ptr);
                        usage();
                        exit(1);
                }
            }
        } else {
            printf("Unknown argument %s\n", ptr);
            usage();
            exit(1);
        }
    }

    if (flag_list)
        rc += a4091_list(addr);

    NewList(&a4091_save.driver_rtask);
    NewList(&a4091_save.driver_wtask);
    NewList(&a4091_save.driver_isrs);

    if (!(flag_config | flag_dma | flag_regs | flag_switches | flag_test |
          flag_kill | flag_zautocfg)) {
        if (flag_list)
            exit(rc);
        usage();
        exit(1);
    }

    if ((addr >= 0x10) && !flag_force && enforcer_check())
        exit(1);

    if (flag_zautocfg) {
        zorro_autoconfig_card();
        exit(0);
    }

    if (addr < 0x10)
        a4091_base = a4091_find(addr);
    else
        a4091_base = addr;

    if (a4091_base == -1) {
        printf("No A4091 cards detected\n");
        exit(1);
    }
    if (a4091_base == A4000T_SCSI_BASE)
        runtime_flags |= FLAG_IS_A4000T;
    if (flag_verbose) {
        printf("%s at 0x%08x\n",
               (runtime_flags & FLAG_IS_A4000T) ? "A4000T" : "A4091",
               a4091_base);
    }

    if (runtime_flags & FLAG_IS_A4000T) {
        a4091_reg_base    = A4000T_SCSI_BASE + A4000T_OFFSET_REGISTERS;
        a4091_rom_base    = 0x00f00000;  // Kickstart ROM base
        a4091_switch_base = A4000T_SCSI_BASE + A4000T_OFFSET_SWITCHES;
    } else {
        a4091_reg_base    = a4091_base + A4091_OFFSET_REGISTERS;
        a4091_rom_base    = a4091_base + A4091_OFFSET_ROM;
        a4091_switch_base = a4091_base + A4091_OFFSET_SWITCHES;
    }

    a4091_save.reg_addr = a4091_reg_base;  // 53C710 registers

    if (flag_kill) {
        BERR_DSACK_SAVE();
        rc += kill_driver();
        BERR_DSACK_RESTORE();
    }

    if (flag_dma || flag_test) {
        /* Check if A4091 is owned by driver */
        attempt_driver_expunge();

        if ((flag_suspend == 0) &&
            (flag_force == 0) &&
            (a4091_show_or_disable_driver_irq_handler(0, 1) +
             a4091_show_or_disable_driver_task(0, 1) > 0)) {
            printf("A4091 driver active in system, which will conflict with "
                   "this tool.\n"
                   "You should use a ROM with no driver. Other options:\n"
                   "  1) \"avail flush\" might remove the open source driver "
                   "if it isn't active.\n"
                   "     Disabling mount of partitions at boot may help with "
                   "this.\n"
                   "  2) \"a4091 -k\" might remove the driver or just crash.\n"
                   "  3) The \"-S\" option will attempt to suspend the driver "
                   "while testing.\n"
                   "  4) The \"-f\" option may be used to ignore the driver "
                   "presence (unsafe).\n");
            exit(1);
        }
        BERR_DSACK_SAVE();
        a4091_state_takeover(flag_suspend);
        BERR_DSACK_RESTORE();
    }

    BERR_DSACK_SAVE();
    do {
        if (flag_loop) {
            if (flag_verbose) {
                printf("Pass %u\n", ++pass);
            } else {
                putchar('.');
            }
        }
        if (flag_config)
            rc += decode_autoconfig();
        if (flag_regs)
            rc += decode_registers();
        if (flag_switches)
            rc += decode_switches();
        if (flag_dma) {
            uint rc2;
            a4091_reset();
            dma_clear_istat();
            rc2 = dma_mem_to_mem(dma[0], dma[1], dma[2]);
            if (rc2 != 0) {
                printf("DMA src=%08x dst=%08x len=%02x failed\n",
                       dma[0], dma[1], dma[2]);
            }
            rc += rc2;
        }
        if (flag_test)
            rc += test_card(test_flags);
        check_break();
    } while ((rc == 0) && flag_loop);

    a4091_state_restore(rc && !(runtime_flags & FLAG_DEBUG));
    BERR_DSACK_RESTORE();

    return (rc);
}
