#include <stdio.h>
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
#include <proto/exec.h>

#include "cmdhandler.h"
#include "nsd.h"
#include "scsi_disk.h"
#include "scsipi_disk.h"
#include "scsipi_all.h"
#include "callout.h"

#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))

#define PAGE_SIZE NBPG
typedef struct device *device_t;

#include "device.h"

#include "scsi_all.h"
#include "scsipiconf.h"
#include "sd.h"
#include "sys_queue.h"
#include "siopreg.h"
#include "siopvar.h"
#include "attach.h"
#include "ndkcompat.h"

#define ADDR8(x)      (volatile uint8_t *)(x)
#define ADDR32(x)     (volatile uint32_t *)(x)

#define BIT(x)        (1 << (x))

extern BOOL __check_abort_enabled;  // 0 = Disable gcc clib2 ^C break handling

a4091_save_t   *asave;
static int      global_opened = 0;
struct IOExtTD *global_tio;
struct MsgPort *global_mp;

#define DEVICE_NAME "a4091.device"

static void
close_exit(void)
{
    if (global_opened) {
        global_opened = 0;
        printf("^C abort\n");
        CloseDevice((struct IORequest *) global_tio);
        DeleteExtIO((struct IORequest *) global_tio);
        DeletePort(global_mp);
    }
    exit(1);
}

static BOOL
is_user_abort(void)
{
    if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        return (1);
    return (0);
}

static void
usage(void)
{
    printf("This tool is used to show " DEVICE_NAME " driver internal state.\n"
           "It does not work on any other driver.\n"
           "Usage:  a4091d [<unit>]\n"
           "        a4091d -p <periph address>\n"
           "        a4091d -x <xs address>\n");
}

typedef const char * const bitdesc_t;

static bitdesc_t bits_periph_flags[] = {
    "REMOVABLE", "MEDIA_LOADED", "WAITING", "OPEN",
        "WAITDRAIN", "GROW_OPENINGS", "MODE_VALID", "RECOVERING",
    "RECOVERING_ACTIVE", "KEEP_LABEL", "SENSE", "UNTAG",
};

static bitdesc_t bits_periph_cap[] = {
    "ANEC", "TERMIOP", "RELADR", "WIDE32",
        "WIDE16", "Bit5", "Bit6", "SYNC",
    "LINKCMDS", "TQING", "SFTRESET", "CMD16",
        "DT", "QAS", "IUS", "Bit15",
};

static bitdesc_t bits_xs_control[] = {
    "NOSLEEP", "POLL", "DISCOVERY", "ASYNC",
        "USERCMD", "SILENT", "IGNORE_NOT_READY", "IGNORE_MEDIA_CHANGE",
    "IGNORE_ILLEGAL_REQUEST", "SILENT_NODEV", "RESET", "DATA_UIO",
        "DATA_IN", "DATA_OUT", "TARGET", "ESCAPE",
    "URGENT", "SIMPLE_TAG", "ORDERED_TAG", "HEAD_TAG",
        "THAW_PERIPH", "FREEZE_PERIPH", "Bit22", "REQSENSE",
};

static bitdesc_t bits_chan_flags[] = {
    "CHAN_OPENINGS", "CHAN_CANGROW", "CHAN_NOSETTLE", "CHAN_TACTIVE",
    "CHAN_RESET_PEND",
};

static bitdesc_t bits_chan_tflags[] = {
    "CHANT_SHUTDOWN", "CHANT_CALLBACK", "CHANT_KICK", "CHANT_GROWRES",
};

static void
print_bits(bitdesc_t *bits, uint nbits, uint value)
{
    uint bit;
    for (bit = 0; value != 0; value >>= 1, bit++) {
        if (value & 1) {
            if ((bit >= nbits) || (bits[bit] == NULL))
                printf(" bit%d", bit);
            else
                printf(" %s", bits[bit]);
        }
    }
}

static const char *
iocmd_name(uint cmd)
{
    static const char *const cmds[] = {
        "CMD_INVALID",      // 0
        "CMD_RESET",        // 1
        "CMD_READ",         // 2
        "CMD_WRITE",        // 3
        "CMD_UPDATE",       // 4
        "CMD_CLEAR",        // 5
        "CMD_STOP",         // 6
        "CMD_START",        // 7
        "CMD_FLUSH",        // 8
        "TD_MOTOR",         // 9  aka CMD_NONSTD
        "TD_SEEK",          // 10
        "TD_FORMAT",        // 11
        "TD_REMOVE",        // 12
        "TD_CHANGENUM",     // 13
        "TD_CHANGESTATE",   // 14
        "TD_PROTSTATUS",    // 15
        "TD_RAWREAD",       // 16
        "TD_RAWRITE",       // 17
        "TD_GETDRIVETYPE",  // 18
        "TD_GETNUMTRACKS",  // 19
        "TD_ADDCHANGEINT",  // 20
        "TD_REMCHANGEINT",  // 21
        "TD_GETGEOMETRY",   // 22
        "TD_EJECT",         // 23
        "TD_READ64",        // 24 aka TD_LASTCOMM
        "TD_WRITE64",       // 25
        "TD_SEEK64",        // 26
        "TD_FORMAT64",      // 27
        "HD_SCSICMD",       // 28
        "MD_SETPARAMS",     // 29
        "30",               // 30
        "31",               // 31
        "CD_INFO",          // 32
        "CD_CONFIG",        // 33
        "CD_TOCMSF",        // 34
        "CD_TOCLSN",        // 35
        "CD_READXL",        // 36
        "CD_PLAYTRACK",     // 37
        "CD_PLAYMSF",       // 38
        "CD_PLAYLSN",       // 39
        "CD_PAUSE",         // 40
        "CD_SEARCH",        // 41
        "CD_QCODEMSF",      // 42
        "CD_QCODELSN",      // 43
        "CD_ATTENUATE",     // 44
        "CD_ADDFRAMEINT",   // 45
        "CD_REMFRAMEINT",   // 45
    };
    if (cmd < ARRAY_SIZE(cmds))
        return (cmds[cmd]);
    switch (cmd) {
        case ETD_READ:          return ("ETD_READ");            // 0x8002
        case ETD_WRITE:         return ("ETD_WRITE");           // 0x8003
        case ETD_UPDATE:        return ("ETD_UPDATE");          // 0x8004
        case ETD_CLEAR:         return ("ETD_CLEAR");           // 0x8005
        case ETD_MOTOR:         return ("ETD_MOTOR");           // 0x8009
        case ETD_SEEK:          return ("ETD_SEEK");            // 0x800a
        case ETD_FORMAT:        return ("ETD_FORMAT");          // 0x800b
        case ETD_RAWREAD:       return ("ETD_RAWREAD");         // 0x8010
        case ETD_RAWWRITE:      return ("ETD_RAWWRITE");        // 0x8011
        case CMD_TERM:          return ("CMD_TERM");            // 0x2ef0
        case CMD_ATTACH:        return ("CMD_ATTACH");          // 0x2ff1
        case CMD_DETACH:        return ("CMD_DETACH");          // 0x2ef2
        case NSCMD_DEVICEQUERY: return ("NSCMD_DEVICEQUERY");   // 0x4000
        case NSCMD_TD_READ64:   return ("NSCMD_TD_READ64");     // 0xc000
        case NSCMD_TD_WRITE64:  return ("NSCMD_TD_WRITE64");    // 0xc001
        case NSCMD_TD_SEEK64:   return ("NSCMD_TD_SEEK64");     // 0xc002
        case NSCMD_TD_FORMAT64: return ("NSCMD_TD_FORMAT64");   // 0xc003
        case NSCMD_ETD_READ64:  return ("NSCMD_ETD_READ64");    // 0xe000
        case NSCMD_ETD_WRITE64: return ("NSCMD_ETD_WRITE64");   // 0xe001
        case NSCMD_ETD_SEEK64:  return ("NSCMD_ETD_SEEK64");    // 0xe002
        case NSCMD_ETD_FORMAT64:return ("NSCMD_ETD_FORMAT64");  // 0xe003
        default:                return ("UNKNOWN");
    }
}

struct ioerr {
    uint        ie_code;
    const char *ie_name;
} ioerrs[] = {
    { -1, "IOERR_OPENFAIL" },
    { -2, "IOERR_ABORTED" },
    { -3, "IOERR_NOCMD" },
    { -4, "IOERR_BADLENGTH" },
    { -5, "IOERR_BADADDRESS" },
    { -6, "IOERR_UNITBUSY" },
    { -7, "IOERR_SELFTEST" },
    {  2, "EACCES" },
    {  5, "EIO" },
    { 12, "ENOMEM" },
    { 16, "EBUSY" },
    { 19, "ENODEV" },
    { 20, "TDERR_NotSpecified" },
    { 21, "TDERR_NoSecHdr" },
    { 22, "TDERR_BadSecPreamble" }, // also EINVAL
    { 23, "TDERR_BadSecID" },
    { 24, "TDERR_BadHdrSum" },
    { 25, "TDERR_BadSecSum" },
    { 26, "TDERR_TooFewSecs" },
    { 27, "TDERR_BadSecHdr" },
    { 28, "TDERR_WriteProt" },      // also ENOSPC
    { 29, "TDERR_DiskChanged" },
    { 30, "TDERR_SeekError" },      // also EROFS
    { 31, "TDERR_NoMem" },
    { 32, "TDERR_BadUnitNum" },
    { 33, "TDERR_BadDriveType" },
    { 34, "TDERR_DriveInUse" },
    { 35, "TDERR_PostReset" },      // also EAGAIN
    { 36, "CDERR_BadDataType" },    // data on disk is wrong type
    { 37, "CDERR_InvalidState" },   // invalid cmd under current conditions
    { 40, "HFERR_SelfUnit" },
    { 41, "HFERR_DMA" },
    { 42, "HFERR_Phase" },
    { 43, "HFERR_Parity" },
    { 44, "HFERR_SelTimeout" },
    { 45, "HFERR_BadStatus" },
    { 46, "ERROR_INQUIRY_FAILED" },
    { 47, "ERROR_TIMEOUT" },
    { 48, "ERROR_BUS_RESET" },
    { 49, "ERROR_TRY_AGAIN" },
    { 56, "HFERR_NoBoard" },
    { 51, "ERROR_BAD_BOARD" },
    { 52, "ERROR_SENSE_CODE" },
};

static void
decode_io_error(uint code)
{
    int pos;
    printf(" code=%u", code);
    for (pos = 0; pos < ARRAY_SIZE(ioerrs); pos++)
        if (code == ioerrs[pos].ie_code) {
            printf(" %s", ioerrs[pos].ie_name);
            break;
        }
}

#define SCSI_VERIFY_10           0x2f
#define SCSI_VERIFY_12           0xaf
#define SCSI_VERIFY_16           0x8f
#define SCSI_UNMAP               0x42
#define SCSI_WRITE_AND_VERIFY_10 0x2e
#define SCSI_WRITE_AND_VERIFY_12 0xae
#define SCSI_WRITE_AND_VERIFY_16 0x8e

static void
decode_scsi_command(const char *indent, uint8_t *cmd, uint cmdlen)
{
    printf("%s ", indent);
    if (cmdlen > 32)
        cmdlen = 32;
    switch (*cmd) {
        case SCSI_TEST_UNIT_READY:  printf(" TEST_UNIT_READY"); // 0x00
            break;
        case SCSI_REZERO_UNIT:      printf(" REZERO");          // 0x01
            break;
        case SCSI_REQUEST_SENSE:    printf(" REQUEST_SENSE");   // 0x03
            break;
        case SCSI_FORMAT_UNIT:      printf(" FORMAT");          // 0x04
            break;
        case SCSI_REASSIGN_BLOCKS:  printf(" REASSIGN");        // 0x07
            break;
        case SCSI_READ_6_COMMAND:   printf(" READ_10");         // 0x08
            goto read_write_6;
        case SCSI_WRITE_6_COMMAND:  printf(" WRITE_6");         // 0x0a
read_write_6:
            printf("(%02x%02x%02x len=%02x)",
                   cmd[1], cmd[2], cmd[3], cmd[4]);
            break;
        case READ_10:   printf(" READ_10");     // 0x28
            goto read_write_10;
        case SCSI_VERIFY_10: printf(" VERIFY_10");   // 0x2f
            goto read_write_10;
        case SCSI_WRITE_AND_VERIFY_10: printf(" WRITE_AND_VERIFY_10"); // 0x2e
            goto read_write_10;
        case WRITE_10:  printf(" WRITE_10");    // 0x2a
read_write_10:
            printf("(%02x%02x%02x%02x len=%02x%02x)",
                   cmd[2], cmd[3], cmd[4], cmd[5], cmd[7], cmd[8]);
            break;
        case READ_12:   printf(" READ_12");     // 0xa8
            goto read_write_12;
        case SCSI_VERIFY_12: printf(" VERIFY_12");   // 0xaf
            goto read_write_12;
        case SCSI_WRITE_AND_VERIFY_12: printf(" WRITE_AND_VERIFY_12"); // 0xae
            goto read_write_12;
        case WRITE_12:  printf(" WRITE_12");    // 0xaa
read_write_12:
            printf("(%02x%02x%02x%02x len=%02x%02x%02x%02x)",
                   cmd[2], cmd[3], cmd[4], cmd[5],
                   cmd[6], cmd[7], cmd[8], cmd[9]);
            break;
        case READ_16:   printf(" READ_16");     // 0x88
            goto read_write_16;
        case SCSI_VERIFY_16: printf(" VERIFY_16");   // 0x8f
            goto read_write_16;
        case SCSI_WRITE_AND_VERIFY_16: printf(" WRITE_AND_VERIFY_16"); // 0x8e
            goto read_write_16;
        case WRITE_16:  printf(" WRITE_16");    // 0x8a
read_write_16:
            printf("(%02x%02x%02x%02x%02x%02x%02x%02x "
                   "len=%02x%02x%02x%02x)",
                   cmd[2], cmd[3], cmd[4], cmd[5],
                   cmd[6], cmd[7], cmd[8], cmd[9],
                   cmd[10], cmd[11], cmd[12], cmd[13]);
            break;
        case SCSI_SEEK_6_COMMAND:    printf(" SEEK_6");           // 0x0b
            printf("(%02x%02x%02x)", cmd[1] & 0x1f, cmd[2], cmd[3]);
            break;
        case SCSI_SEEK_10_COMMAND:   printf(" SEEK_10");          // 0x2b
            printf("(%02x%02x%02x%02x lun=%x)", cmd[2], cmd[3], cmd[4], cmd[5], cmd[1]);
            break;
        case INQUIRY:                printf(" INQUIRY");          // 0x12
            break;
        case SCSI_MODE_SELECT_6:     printf(" MODE_SELECT_6");    // 0x15
            break;
        case SCSI_MODE_SELECT_10:    printf(" MODE_SELECT_10");   // 0x55
            break;
        case SCSI_RESERVE_6:         printf(" SCSI_RESERVE_6");   // 0x16
            break;
        case SCSI_RELEASE_6:         printf(" SCSI_RELEASE_6");   // 0x17
            break;
        case SCSI_RESERVE_10:        printf(" SCSI_RESERVE_10");  // 0x56
            break;
        case SCSI_RELEASE_10:        printf(" SCSI_RELEASE_10");  // 0x57
            break;
        case SCSI_MODE_SENSE_6:      printf(" MODE_SENSE_6");     // 0x1a
            goto mode_sense_6_10;
        case SCSI_MODE_SENSE_10:     printf(" MODE_SENSE_10");    // 0x5a
mode_sense_6_10:
            printf("(%02x)", cmd[2]);  // page code
            break;
#define SSS_START               0x01
#define SSS_LOEJ                0x02
        case START_STOP:             printf(" START_STOP");       // 0x1b
            printf(" %s", (cmd[4] & SSS_LOEJ) ?
                         ((cmd[4] & SSS_START) ? "Load" : "Eject") :
                         ((cmd[4] & SSS_START) ? "Start" : "Stop"));
            break;
        case SCSI_SEND_DIAGNOSTIC:   printf(" SEND_DIAGNOSTIC");  // 0x1d
            break;
        case READ_FORMAT_CAPACITIES: printf(" READ_FORMAT_CAPACITIES"); // 0x23
            break;
        case READ_CAPACITY_10:       printf(" READ_CAPACITY_10"); // 0x25
            break;
        case SERVICE_ACTION_IN:                                   // 0x9e
            if (cmd[1] == SRC16_READ_CAPACITY)  // 0x10
                printf(" READ_CAPACITY_16");
            else
                printf(" SERVICE_ACTION_IN %02x", cmd[1]);
            break;
        case SCSI_SYNCHRONIZE_CACHE_10:  printf(" SYNC_CACHE10"); // 0x36
            goto read_write_10;  // addr=2..5 len=7..8
        case SCSI_SYNCHRONIZE_CACHE_16:  printf(" SYNC_CACHE16"); // 0x36
            goto read_write_16;  // addr=2..9 len=10..13
        case SCSI_READ_DEFECT_DATA:  printf(" READ_DEFECT");      // 0x37
            break;
        case SCSI_UNMAP:             printf("  UNMAP");           // 0x42
            break;
        default:
            printf(" Unknown cmd ");
            break;
    }
    while (cmdlen-- > 0) {
        printf(" %02x", *(cmd++));
    }
    printf("\n");
}

static void
decode_io_command(int indent_count, struct IOStdReq *ior)
{
    char indent[32];
    sprintf(indent, "%*s", indent_count, "");

    switch (ior->io_Command) {
        case HD_SCSICMD: {
            struct SCSICmd *sc = ior->io_Data;
            if (sc == NULL)
                return;
            printf("%sscsi_Data=%p\n", indent, sc->scsi_Data);
            printf("%sscsi_Length=%08"PRIx32"\n", indent, sc->scsi_Length);
            printf("%sscsi_Actual=%08"PRIx32"\n", indent, sc->scsi_Actual);
            printf("%sscsi_Command=%p\n", indent, sc->scsi_Command);
            if (sc->scsi_CmdLength > 0) {
                decode_scsi_command(indent, (uint8_t *)sc->scsi_Command,
                                    sc->scsi_CmdLength);
            }
            printf("%sscsi_CmdLength=%04x\n", indent, sc->scsi_CmdLength);
            printf("%sscsi_CmdActual=%04x\n", indent, sc->scsi_CmdActual);
            printf("%sscsi_Flags=%02x\n", indent, sc->scsi_Flags);
            printf("%sscsi_Status=%02x\n", indent, sc->scsi_Status);
            printf("%sscsi_SenseData=%p\n", indent, sc->scsi_SenseData);
            printf("%sscsi_SenseLength=%02x\n",
                   indent, sc->scsi_SenseLength);
            printf("%sscsi_SenseActual=%02x\n",
                   indent, sc->scsi_SenseActual);
        }
    }
}

static const char *
decode_xs_error(int error)
{
    static const char *const errs[] = {
        "XS_NOERROR",           // 0 No error
        "XS_SENSE",             // 1 Check the returned sense for the error
        "XS_SHORTSENSE",        // 2 Check the ATAPI sense for the error
        "XS_DRIVER_STUFFUP",    // 3 Driver failed to perform operation
        "XS_RESOURCE_SHORTAGE", // 4 adapter resource shortage
        "XS_SELTIMEOUT",        // 5 The device timed out.. turned off?
        "XS_TIMEOUT",           // 6 The Timeout reported was caught by SW
        "XS_BUSY",              // 7 The device busy, try again later?
        "XS_RESET",             // 8 bus was reset; possible retry command
        "XS_REQUEUE",           // 9 requeue this command
    };
    if (error < ARRAY_SIZE(errs))
        return (errs[error]);
    return ("UNKNOWN");
}

static const char *
decode_scsi_sense_key(int key)
{
    static const char *const keys[] = {
        "NO SENSE",          // 0x0 SKEY_NO_SENSE
        "RECOVERED ERROR",   // 0x1 SKEY_RECOVERED_ERROR
        "NOT READY",         // 0x2 SKEY_NOT_READY
        "MEDIUM ERROR",      // 0x3 SKEY_MEDIUM_ERROR
        "HARDWARE ERROR",    // 0x4 SKEY_HARDWARE_ERROR
        "ILLEGAL REQUEST",   // 0x5 SKEY_ILLEGAL_REQUEST
        "UNIT ATTENTION",    // 0x6 SKEY_UNIT_ATTENTION
        "DATA PROTECT",      // 0x7 SKEY_DATA_PROTECT
        "BLANK CHECK",       // 0x8 SKEY_BLANK_CHECK
        "VENDOR SPECIFIC",   // 0x9 SKEY_VENDOR_SPECIFIC
        "COPY ABORTED",      // 0xa SKEY_COPY_ABORTED
        "ABORTED COMMAND",   // 0xb SKEY_ABORTED_COMMAND
        "EQUAL",             // 0xc SKEY_EQUAL              Obsolete
        "VOLUME OVERFLOW",   // 0xd SKEY_VOLUME_OVERFLOW
        "MISCOMPARE",        // 0xe SKEY_MISCOMPARE
        "COMPLETED",         // 0xf
    };
    if (key < ARRAY_SIZE(keys))
        return (keys[key]);
    return ("UNKNOWN");
}

#define SCSI_COND_MET       0x04
#define SCSI_INTERM_CNDMET  0x14
#define SCSI_TASK_ABORTED   0x40

static const char *
decode_xs_status(int status)
{
    switch (status) {
        case SCSI_OK:            return ("OK");              // 0x00
        case SCSI_CHECK:         return ("CHECK");           // 0x02
        case SCSI_COND_MET:      return ("COND_MET");        // 0x04
        case SCSI_BUSY:          return ("BUSY");            // 0x08
        case SCSI_INTERM:        return ("INTERMEDIATE");    // 0x10
        case SCSI_INTERM_CNDMET: return ("INTERM_COND_MET"); // 0x14
        case SCSI_RESV_CONFLICT: return ("RESV_CONFLICT");   // 0x18
        case SCSI_TERMINATED:    return ("TERMINATED");      // 0x22
        case SCSI_QUEUE_FULL:    return ("QUEUE_FULL");      // 0x28
        case SCSI_ACA_ACTIVE:    return ("ACA_ACTIVE");      // 0x30
        case SCSI_TASK_ABORTED:  return ("TASK_ABORTED");    // 0x40
        default:                 return ("UNKNOWN");
    }
}

/*
 * Additional status codes (sense.asc)
 * -----------------------------------
 *          D - Direct Access Block Device (SBC-4)           device column key
 *          .Z - Host Managed Zoned Block Device (ZBC)     --------------------
 *          . T - Sequential Access Device (SSC-5)             blank = reserved
 *          .  P - Processor Device (SPC-2)                not blank = allowed
 *          .  .R - C/DVD Device (MMC-6)
 *          .  . O - Optical Memory Block Device (SBC)
 *          .  .  M - Media Changer Device (SMC-3)
 *          .  .  .A - Storage Array Device (SCC-2)
 *          .  .  . E - SCSI Enclosure Services device (SES-3)
 *          .  .  .  B - Simplified Direct-Access (Reduced Block) device (RBC)
 *          .  .  .  .K - Optical Card Reader/Writer device (OCRW)
 *          .  .  .  . V - Automation/Device Interface device (ADC-4)
 *          .  .  .  .  F - Object-based Storage Device (OSD-2)
 * ASC/ASCQ DZTPROMAEBKVF  Description
 * -------- -------------  ----------------------------------------------------
 * 00h/00h  DZTPROMAEBKVF  NO ADDITIONAL SENSE INFORMATION
 * 00h/01h    T            FILEMARK DETECTED
 * 00h/02h    T            END-OF-PARTITION/MEDIUM DETECTED
 * 00h/03h    T            SETMARK DETECTED
 * 00h/04h    T            BEGINNING-OF-PARTITION/MEDIUM DETECTED
 * 00h/05h    T            END-OF-DATA DETECTED
 * 00h/06h  DZTPROMAEBKVF  I/O PROCESS TERMINATED
 * 00h/07h    T            PROGRAMMABLE EARLY WARNING DETECTED
 * 00h/11h      R          AUDIO PLAY OPERATION IN PROGRESS
 * 00h/12h      R          AUDIO PLAY OPERATION PAUSED
 * 00h/13h      R          AUDIO PLAY OPERATION SUCCESSFULLY COMPLETED
 * 00h/14h      R          AUDIO PLAY OPERATION STOPPED DUE TO ERROR
 * 00h/15h      R          NO CURRENT AUDIO STATUS TO RETURN
 * 00h/16h  DZTPROMAEBKVF  OPERATION IN PROGRESS
 * 00h/17h  DZT ROMAEBKVF  CLEANING REQUESTED
 * 00h/18h    T            ERASE OPERATION IN PROGRESS
 * 00h/19h    T            LOCATE OPERATION IN PROGRESS
 * 00h/1Ah    T            REWIND OPERATION IN PROGRESS
 * 00h/1Bh    T            SET CAPACITY OPERATION IN PROGRESS
 * 00h/1Ch    T            VERIFY OPERATION IN PROGRESS
 * 00h/1Dh  DZT      B     ATA PASS THROUGH INFORMATION AVAILABLE
 * 00h/1Eh  DZT R MAEBKV   CONFLICTING SA CREATION REQUEST
 * 00h/1Fh  DZT      B     LOGICAL UNIT TRANSITIONING TO ANOTHER POWER CONDITION
 * 00h/20h  DZTP     B     EXTENDED COPY INFORMATION AVAILABLE
 * 00h/21h  DZ             ATOMIC COMMAND ABORTED DUE TO ACA
 * 00h/22h  DZ             DEFERRED MICROCODE IS PENDING
 * 01h/00h  DZ   O   BK    NO INDEX/SECTOR SIGNAL
 * 02h/00h  DZ  RO   BK    NO SEEK COMPLETE
 * 03h/00h  DZT  O   BK    PERIPHERAL DEVICE WRITE FAULT
 * 03h/01h    T            NO WRITE CURRENT
 * 03h/02h    T            EXCESSIVE WRITE ERRORS
 * 04h/00h  DZTPROMAEBKVF  LOGICAL UNIT NOT READY, CAUSE NOT REPORTABLE
 * 04h/01h  DZTPROMAEBKVF  LOGICAL UNIT IS IN PROCESS OF BECOMING READY
 * 04h/02h  DZTPROMAEBKVF  LOGICAL UNIT NOT READY, INITIALIZING COMMAND REQUIRED
 * 04h/03h  DZTPROMAEBKVF  LOGICAL UNIT NOT READY, MANUAL INTERVENTION REQUIRED
 * 04h/04h  DZT RO   B     LOGICAL UNIT NOT READY, FORMAT IN PROGRESS
 * 04h/05h  DZT  O A BK F  LOGICAL UNIT NOT READY, REBUILD IN PROGRESS
 * 04h/06h  DZT  O A BK    LOGICAL UNIT NOT READY, RECALCULATION IN PROGRESS
 * 04h/07h  DZTPROMAEBKVF  LOGICAL UNIT NOT READY, OPERATION IN PROGRESS
 * 04h/08h      R          LOGICAL UNIT NOT READY, LONG WRITE IN PROGRESS
 * 04h/09h  DZTPROMAEBKVF  LOGICAL UNIT NOT READY, SELF-TEST IN PROGRESS
 * 04h/0Ah  DZTPROMAEBKVF  LOGICAL UNIT NOT ACCESSIBLE, ASYMMETRIC ACCESS STATE TRANSITION
 * 04h/0Bh  DZTPROMAEBKVF  LOGICAL UNIT NOT ACCESSIBLE, TARGET PORT IN STANDBY STATE
 * 04h/0Ch  DZTPROMAEBKVF  LOGICAL UNIT NOT ACCESSIBLE, TARGET PORT IN UNAVAILABLE STATE
 * 04h/0Dh              F  LOGICAL UNIT NOT READY, STRUCTURE CHECK REQUIRED
 * 04h/0Eh  DZT R MAEBKVF  LOGICAL UNIT NOT READY, SECURITY SESSION IN PROGRESS
 * 04h/10h  DZT ROM  B     LOGICAL UNIT NOT READY, AUXILIARY MEMORY NOT ACCESSIBLE
 * 04h/11h  DZT RO AEB VF  LOGICAL UNIT NOT READY, NOTIFY (ENABLE SPINUP) REQUIRED
 * 04h/12h        M    V   LOGICAL UNIT NOT READY, OFFLINE
 * 04h/13h  DZT R MAEBKV   LOGICAL UNIT NOT READY, SA CREATION IN PROGRESS
 * 04h/14h  DZ       B     LOGICAL UNIT NOT READY, SPACE ALLOCATION IN PROGRESS
 * 04h/15h        M        LOGICAL UNIT NOT READY, ROBOTICS DISABLED
 * 04h/16h        M        LOGICAL UNIT NOT READY, CONFIGURATION REQUIRED
 * 04h/17h        M        LOGICAL UNIT NOT READY, CALIBRATION REQUIRED
 * 04h/18h        M        LOGICAL UNIT NOT READY, A DOOR IS OPEN
 * 04h/19h        M        LOGICAL UNIT NOT READY, OPERATING IN SEQUENTIAL MODE
 * 04h/1Ah  DZ       B     LOGICAL UNIT NOT READY, START STOP UNIT COMMAND IN PROGRESS
 * 04h/1Bh  DZ       B     LOGICAL UNIT NOT READY, SANITIZE IN PROGRESS
 * 04h/1Ch  DZT   MAEB     LOGICAL UNIT NOT READY, ADDITIONAL POWER USE NOT YET GRANTED
 * 04h/1Dh  DZ             LOGICAL UNIT NOT READY, CONFIGURATION IN PROGRESS
 * 04h/1Eh  DZ             LOGICAL UNIT NOT READY, MICROCODE ACTIVATION REQUIRED
 * 04h/1Fh  DZTPROMAEBKVF  LOGICAL UNIT NOT READY, MICROCODE DOWNLOAD REQUIRED
 * 04h/20h  DZTPROMAEBKVF  LOGICAL UNIT NOT READY, LOGICAL UNIT RESET REQUIRED
 * 04h/21h  DZTPROMAEBKVF  LOGICAL UNIT NOT READY, HARD RESET REQUIRED
 * 04h/22h  DZTPROMAEBKVF  LOGICAL UNIT NOT READY, POWER CYCLE REQUIRED
 * 04h/23h  DZ             LOGICAL UNIT NOT READY, AFFILIATION REQUIRED
 * 04h/24h  DZ             DEPOPULATION IN PROGRESS
 * 04h/25h  DZ             DEPOPULATION RESTORATION IN PROGRESS
 * 05h/00h  DZT ROMAEBKVF  LOGICAL UNIT DOES NOT RESPOND TO SELECTION
 * 06h/00h  DZ  ROM  BK    NO REFERENCE POSITION FOUND
 * 07h/00h  DZT ROM  BK    MULTIPLE PERIPHERAL DEVICES SELECTED
 * 08h/00h  DZT ROMAEBKVF  LOGICAL UNIT COMMUNICATION FAILURE
 * 08h/01h  DZT ROMAEBKVF  LOGICAL UNIT COMMUNICATION TIME-OUT
 * 08h/02h  DZT ROMAEBKVF  LOGICAL UNIT COMMUNICATION PARITY ERROR
 * 08h/03h  DZT ROM  BK    LOGICAL UNIT COMMUNICATION CRC ERROR (ULTRA-DMA/32)
 * 08h/04h  DZTPRO    K    UNREACHABLE COPY TARGET
 * 09h/00h  DZT RO   B     TRACK FOLLOWING ERROR
 * 09h/01h      RO    K    TRACKING SERVO FAILURE
 * 09h/02h      RO    K    FOCUS SERVO FAILURE
 * 09h/03h      RO         SPINDLE SERVO FAILURE
 * 09h/04h  DZT RO   B     HEAD SELECT FAULT
 * 09h/05h  DZT RO   B     VIBRATION INDUCED TRACKING ERROR
 * 0Ah/00h  DZTPROMAEBKVF  ERROR LOG OVERFLOW
 * 0Bh/00h  DZTPROMAEBKVF  WARNING
 * 0Bh/01h  DZTPROMAEBKVF  WARNING - SPECIFIED TEMPERATURE EXCEEDED
 * 0Bh/02h  DZTPROMAEBKVF  WARNING - ENCLOSURE DEGRADED
 * 0Bh/03h  DZTPROMAEBKVF  WARNING - BACKGROUND SELF-TEST FAILED
 * 0Bh/04h  DZTPRO AEBKVF  WARNING - BACKGROUND PRE-SCAN DETECTED MEDIUM ERROR
 * 0Bh/05h  DZTPRO AEBKVF  WARNING - BACKGROUND MEDIUM SCAN DETECTED MEDIUM ERROR
 * 0Bh/06h  DZTPROMAEBKVF  WARNING - NON-VOLATILE CACHE NOW VOLATILE
 * 0Bh/07h  DZTPROMAEBKVF  WARNING - DEGRADED POWER TO NON-VOLATILE CACHE
 * 0Bh/08h  DZTPROMAEBKVF  WARNING - POWER LOSS EXPECTED
 * 0Bh/09h  DZ             WARNING - DEVICE STATISTICS NOTIFICATION ACTIVE
 * 0Bh/0Ah  DZTPROMAEBKV   WARNING - HIGH CRITICAL TEMPERATURE LIMIT EXCEEDED
 * 0Bh/0Bh  DZTPROMAEBKV   WARNING - LOW CRITICAL TEMPERATURE LIMIT EXCEEDED
 * 0Bh/0Ch  DZTPROMAEBKV   WARNING - HIGH OPERATING TEMPERATURE LIMIT EXCEEDED
 * 0Bh/0Dh  DZTPROMAEBKV   WARNING - LOW OPERATING TEMPERATURE LIMIT EXCEEDED
 * 0Bh/0Eh  DZTPROMAEBKV   WARNING - HIGH CRITICAL HUMIDITY LIMIT EXCEEDED
 * 0Bh/0Fh  DZTPROMAEBKV   WARNING - LOW CRITICAL HUMIDITY LIMIT EXCEEDED
 * 0Bh/10h  DZTPROMAEBKV   WARNING - HIGH OPERATING HUMIDITY LIMIT EXCEEDED
 * 0Bh/11h  DZTPROMAEBKV   WARNING - LOW OPERATING HUMIDITY LIMIT EXCEEDED
 * 0Bh/12h  DZTPROMAEBKVF  WARNING - MICROCODE SECURITY AT RISK
 * 0Bh/13h  DZTPROMAEBKVF  WARNING - MICROCODE DIGITAL SIGNATURE VALIDATION FAILURE
 * 0Bh/14h  DZ             WARNING - PHYSICAL ELEMENT STATUS CHANGE
 * 0Ch/00h  DZT R          WRITE ERROR
 * 0Ch/01h  DZ        K    WRITE ERROR - RECOVERED WITH AUTO REALLOCATION
 * 0Ch/02h  DZ   O   BK    WRITE ERROR - AUTO REALLOCATION FAILED
 * 0Ch/03h  DZ   O   BK    WRITE ERROR - RECOMMEND REASSIGNMENT
 * 0Ch/04h  DZT  O   B     COMPRESSION CHECK MISCOMPARE ERROR
 * 0Ch/05h  DZT  O   B     DATA EXPANSION OCCURRED DURING COMPRESSION
 * 0Ch/06h  DZT  O   B     BLOCK NOT COMPRESSIBLE
 * 0Ch/07h  DZ  R          WRITE ERROR - RECOVERY NEEDED
 * 0Ch/08h      R          WRITE ERROR - RECOVERY FAILED
 * 0Ch/09h      R          WRITE ERROR - LOSS OF STREAMING
 * 0Ch/0Ah      R          WRITE ERROR - PADDING BLOCKS ADDED
 * 0Ch/0Bh  DZT ROM  B     AUXILIARY MEMORY WRITE ERROR
 * 0Ch/0Ch  DZTPRO AEBKVF  WRITE ERROR - UNEXPECTED UNSOLICITED DATA
 * 0Ch/0Dh  DZTPRO AEBKVF  WRITE ERROR - NOT ENOUGH UNSOLICITED DATA
 * 0Ch/0Eh  DZT  O   BK    MULTIPLE WRITE ERRORS
 * 0Ch/0Fh      R          DEFECTS IN ERROR WINDOW
 * 0Ch/10h  DZ             INCOMPLETE MULTIPLE ATOMIC WRITE OPERATIONS
 * 0Ch/11h  DZ             WRITE ERROR - RECOVERY SCAN NEEDED
 * 0Ch/12h  DZ             WRITE ERROR - INSUFFICIENT ZONE RESOURCES
 * 0Dh/00h  DZTPRO A  K    ERROR DETECTED BY THIRD PARTY TEMPORARY INITIATOR
 * 0Dh/01h  DZTPRO A  K    THIRD PARTY DEVICE FAILURE
 * 0Dh/02h  DZTPRO A  K    COPY TARGET DEVICE NOT REACHABLE
 * 0Dh/03h  DZTPRO A  K    INCORRECT COPY TARGET DEVICE TYPE
 * 0Dh/04h  DZTPRO A  K    COPY TARGET DEVICE DATA UNDERRUN
 * 0Dh/05h  DZTPRO A  K    COPY TARGET DEVICE DATA OVERRUN
 * 0Eh/00h  DZTPROMAEBK F  INVALID INFORMATION UNIT
 * 0Eh/01h  DZTPROMAEBK F  INFORMATION UNIT TOO SHORT
 * 0Eh/02h  DZTPROMAEBK F  INFORMATION UNIT TOO LONG
 * 0Eh/03h  DZTPR MAEBK F  INVALID FIELD IN COMMAND INFORMATION UNIT
 * 0Fh/00h
 * 10h/00h  DZ   O   BK    ID CRC OR ECC ERROR
 * 10h/01h  DZT  O         LOGICAL BLOCK GUARD CHECK FAILED
 * 10h/02h  DZT  O         LOGICAL BLOCK APPLICATION TAG CHECK FAILED
 * 10h/03h  DZT  O         LOGICAL BLOCK REFERENCE TAG CHECK FAILED
 * 10h/04h    T            LOGICAL BLOCK PROTECTION ERROR ON RECOVER BUFFERED DATA
 * 10h/05h    T            LOGICAL BLOCK PROTECTION METHOD ERROR
 * 11h/00h  DZT RO   BK    UNRECOVERED READ ERROR
 * 11h/01h  DZT RO   BK    READ RETRIES EXHAUSTED
 * 11h/02h  DZT RO   BK    ERROR TOO LONG TO CORRECT
 * 11h/03h  DZT  O   BK    MULTIPLE READ ERRORS
 * 11h/04h  DZ   O   BK    UNRECOVERED READ ERROR - AUTO REALLOCATE FAILED
 * 11h/05h      RO   B     L-EC UNCORRECTABLE ERROR
 * 11h/06h      RO   B     CIRC UNRECOVERED ERROR
 * 11h/07h       O   B     DATA RE-SYNCHRONIZATION ERROR
 * 11h/08h    T            INCOMPLETE BLOCK READ
 * 11h/09h    T            NO GAP FOUND
 * 11h/0Ah  DZT  O   BK    MISCORRECTED ERROR
 * 11h/0Bh  DZ   O   BK    UNRECOVERED READ ERROR - RECOMMEND REASSIGNMENT
 * 11h/0Ch  DZ   O   BK    UNRECOVERED READ ERROR - RECOMMEND REWRITE THE DATA
 * 11h/0Dh  DZT RO   B     DE-COMPRESSION CRC ERROR
 * 11h/0Eh  DZT RO   B     CANNOT DECOMPRESS USING DECLARED ALGORITHM
 * 11h/0Fh      R          ERROR READING UPC/EAN NUMBER
 * 11h/10h      R          ERROR READING ISRC NUMBER
 * 11h/11h      R          READ ERROR - LOSS OF STREAMING
 * 11h/12h  DZT ROM  B     AUXILIARY MEMORY READ ERROR
 * 11h/13h  DZTPRO AEBKVF  READ ERROR - FAILED RETRANSMISSION REQUEST
 * 11h/14h  DZ             READ ERROR - LBA MARKED BAD BY APPLICATION CLIENT
 * 11h/15h  DZ             WRITE AFTER SANITIZE REQUIRED
 * 12h/00h  DZ   O   BK    ADDRESS MARK NOT FOUND FOR ID FIELD
 * 13h/00h  DZ   O   BK    ADDRESS MARK NOT FOUND FOR DATA FIELD
 * 14h/00h  DZT RO   BK    RECORDED ENTITY NOT FOUND
 * 14h/01h  DZT RO   BK    RECORD NOT FOUND
 * 14h/02h    T            FILEMARK OR SETMARK NOT FOUND
 * 14h/03h    T            END-OF-DATA NOT FOUND
 * 14h/04h    T            BLOCK SEQUENCE ERROR
 * 14h/05h  DZT  O   BK    RECORD NOT FOUND - RECOMMEND REASSIGNMENT
 * 14h/06h  DZT  O   BK    RECORD NOT FOUND - DATA AUTO-REALLOCATED
 * 14h/07h    T            LOCATE OPERATION FAILURE
 * 15h/00h  DZT ROM  BK    RANDOM POSITIONING ERROR
 * 15h/01h  DZT ROM  BK    MECHANICAL POSITIONING ERROR
 * 15h/02h  DZT RO   BK    POSITIONING ERROR DETECTED BY READ OF MEDIUM
 * 16h/00h  DZ   O   BK    DATA SYNCHRONIZATION MARK ERROR
 * 16h/01h  DZ   O   BK    DATA SYNC ERROR - DATA REWRITTEN
 * 16h/02h  DZ   O   BK    DATA SYNC ERROR - RECOMMEND REWRITE
 * 16h/03h  DZ   O   BK    DATA SYNC ERROR - DATA AUTO-REALLOCATED
 * 16h/04h  DZ   O   BK    DATA SYNC ERROR - RECOMMEND REASSIGNMENT
 * 17h/00h  DZT RO   BK    RECOVERED DATA WITH NO ERROR CORRECTION APPLIED
 * 17h/01h  DZT RO   BK    RECOVERED DATA WITH RETRIES
 * 17h/02h  DZT RO   BK    RECOVERED DATA WITH POSITIVE HEAD OFFSET
 * 17h/03h  DZT RO   BK    RECOVERED DATA WITH NEGATIVE HEAD OFFSET
 * 17h/04h      RO   B     RECOVERED DATA WITH RETRIES AND/OR CIRC APPLIED
 * 17h/05h  DZ  RO   BK    RECOVERED DATA USING PREVIOUS SECTOR ID
 * 17h/06h  DZ   O   BK    RECOVERED DATA WITHOUT ECC - DATA AUTO-REALLOCATED
 * 17h/07h  DZ  RO   BK    RECOVERED DATA WITHOUT ECC - RECOMMEND REASSIGNMENT
 * 17h/08h  DZ  RO   BK    RECOVERED DATA WITHOUT ECC - RECOMMEND REWRITE
 * 17h/09h  DZ  RO   BK    RECOVERED DATA WITHOUT ECC - DATA REWRITTEN
 * 18h/00h  DZT RO   BK    RECOVERED DATA WITH ERROR CORRECTION APPLIED
 * 18h/01h  DZ  RO   BK    RECOVERED DATA WITH ERROR CORR. & RETRIES APPLIED
 * 18h/02h  DZ  RO   BK    RECOVERED DATA - DATA AUTO-REALLOCATED
 * 18h/03h      R          RECOVERED DATA WITH CIRC
 * 18h/04h      R          RECOVERED DATA WITH L-EC
 * 18h/05h  DZ  RO   BK    RECOVERED DATA - RECOMMEND REASSIGNMENT
 * 18h/06h  DZ  RO   BK    RECOVERED DATA - RECOMMEND REWRITE
 * 18h/07h  DZ   O   BK    RECOVERED DATA WITH ECC - DATA REWRITTEN
 * 18h/08h      R          RECOVERED DATA WITH LINKING
 * 19h/00h  DZ   O    K    DEFECT LIST ERROR
 * 19h/01h  DZ   O    K    DEFECT LIST NOT AVAILABLE
 * 19h/02h  DZ   O    K    DEFECT LIST ERROR IN PRIMARY LIST
 * 19h/03h  DZ   O    K    DEFECT LIST ERROR IN GROWN LIST
 * 1Ah/00h  DZTPROMAEBKVF  PARAMETER LIST LENGTH ERROR
 * 1Bh/00h  DZTPROMAEBKVF  SYNCHRONOUS DATA TRANSFER ERROR
 * 1Ch/00h  DZ   O   BK    DEFECT LIST NOT FOUND
 * 1Ch/01h  DZ   O   BK    PRIMARY DEFECT LIST NOT FOUND
 * 1Ch/02h  DZ   O   BK    GROWN DEFECT LIST NOT FOUND
 * 1Dh/00h  DZT RO   BK    MISCOMPARE DURING VERIFY OPERATION
 * 1Dh/01h  DZ       B     MISCOMPARE VERIFY OF UNMAPPED LBA
 * 1Eh/00h  DZ   O   BK    RECOVERED ID WITH ECC CORRECTION
 * 1Fh/00h  DZ   O    K    PARTIAL DEFECT LIST TRANSFER
 * 20h/00h  DZTPROMAEBKVF  INVALID COMMAND OPERATION CODE
 * 20h/01h  DZTPROMAEBK    ACCESS DENIED - INITIATOR PENDING-ENROLLED
 * 20h/02h  DZTPROMAEBK    ACCESS DENIED - NO ACCESS RIGHTS
 * 20h/03h  DZTPROMAEBK    ACCESS DENIED - INVALID MGMT ID KEY
 * 20h/04h    T            ILLEGAL COMMAND WHILE IN WRITE CAPABLE STATE
 * 20h/05h    T            Obsolete
 * 20h/06h    T            ILLEGAL COMMAND WHILE IN EXPLICIT ADDRESS MODE
 * 20h/07h    T            ILLEGAL COMMAND WHILE IN IMPLICIT ADDRESS MODE
 * 20h/08h  DZTPROMAEBK    ACCESS DENIED - ENROLLMENT CONFLICT
 * 20h/09h  DZTPROMAEBK    ACCESS DENIED - INVALID LU IDENTIFIER
 * 20h/0Ah  DZTPROMAEBK    ACCESS DENIED - INVALID PROXY TOKEN
 * 20h/0Bh  DZTPROMAEBK    ACCESS DENIED - ACL LUN CONFLICT
 * 20h/0Ch    T            ILLEGAL COMMAND WHEN NOT IN APPEND-ONLY MODE
 * 20h/0Dh  D              NOT AN ADMINISTRATIVE LOGICAL UNIT
 * 20h/0Eh  D              NOT A SUBSIDIARY LOGICAL UNIT
 * 20h/0Fh  D              NOT A CONGLOMERATE LOGICAL UNIT
 * 21h/00h  DZT RO   BK    LOGICAL BLOCK ADDRESS OUT OF RANGE
 * 21h/01h  DZT ROM  BK    INVALID ELEMENT ADDRESS
 * 21h/02h      R          INVALID ADDRESS FOR WRITE
 * 21h/03h      R          INVALID WRITE CROSSING LAYER JUMP
 * 21h/04h  DZ             UNALIGNED WRITE COMMAND
 * 21h/05h  DZ             WRITE BOUNDARY VIOLATION
 * 21h/06h  DZ             ATTEMPT TO READ INVALID DATA
 * 21h/07h  DZ             READ BOUNDARY VIOLATION
 * 21h/08h  DZ             MISALIGNED WRITE COMMAND
 * 21h/09h  DZ             ATTEMPT TO ACCESS GAP ZONE
 * 22h/00h  DZ             ILLEGAL FUNCTION (USE 20 00, 24 00, OR 26 00)
 * 23h/00h  DZTP     B     INVALID TOKEN OPERATION, CAUSE NOT REPORTABLE
 * 23h/01h  DZTP     B     INVALID TOKEN OPERATION, UNSUPPORTED TOKEN TYPE
 * 23h/02h  DZTP     B     INVALID TOKEN OPERATION, REMOTE TOKEN USAGE NOT SUPPORTED
 * 23h/03h  DZTP     B     INVALID TOKEN OPERATION, REMOTE ROD TOKEN CREATION NOT SUPPORTED
 * 23h/04h  DZTP     B     INVALID TOKEN OPERATION, TOKEN UNKNOWN
 * 23h/05h  DZTP     B     INVALID TOKEN OPERATION, TOKEN CORRUPT
 * 23h/06h  DZTP     B     INVALID TOKEN OPERATION, TOKEN REVOKED
 * 23h/07h  DZTP     B     INVALID TOKEN OPERATION, TOKEN EXPIRED
 * 23h/08h  DZTP     B     INVALID TOKEN OPERATION, TOKEN CANCELLED
 * 23h/09h  DZTP     B     INVALID TOKEN OPERATION, TOKEN DELETED
 * 23h/0Ah  DZTP     B     INVALID TOKEN OPERATION, INVALID TOKEN LENGTH
 * 24h/00h  DZTPROMAEBKVF  INVALID FIELD IN CDB
 * 24h/01h  DZTPRO AEBKVF  CDB DECRYPTION ERROR
 * 24h/02h    T            Obsolete
 * 24h/03h    T            Obsolete
 * 24h/04h              F  SECURITY AUDIT VALUE FROZEN
 * 24h/05h              F  SECURITY WORKING KEY FROZEN
 * 24h/06h              F  NONCE NOT UNIQUE
 * 24h/07h              F  NONCE TIMESTAMP OUT OF RANGE
 * 24h/08h  DZT R MAEBKV   INVALID XCDB
 * 24h/09h  DZ             INVALID FAST FORMAT
 * 25h/00h  DZTPROMAEBKVF  LOGICAL UNIT NOT SUPPORTED
 * 26h/00h  DZTPROMAEBKVF  INVALID FIELD IN PARAMETER LIST
 * 26h/01h  DZTPROMAEBKVF  PARAMETER NOT SUPPORTED
 * 26h/02h  DZTPROMAEBKVF  PARAMETER VALUE INVALID
 * 26h/03h  DZTPROMAE K    THRESHOLD PARAMETERS NOT SUPPORTED
 * 26h/04h  DZTPROMAEBKVF  INVALID RELEASE OF PERSISTENT RESERVATION
 * 26h/05h  DZTPRO A BK    DATA DECRYPTION ERROR
 * 26h/06h  DZTPRO    K    TOO MANY TARGET DESCRIPTORS
 * 26h/07h  DZTPRO    K    UNSUPPORTED TARGET DESCRIPTOR TYPE CODE
 * 26h/08h  DZTPRO    K    TOO MANY SEGMENT DESCRIPTORS
 * 26h/09h  DZTPRO    K    UNSUPPORTED SEGMENT DESCRIPTOR TYPE CODE
 * 26h/0Ah  DZTPRO    K    UNEXPECTED INEXACT SEGMENT
 * 26h/0Bh  DZTPRO    K    INLINE DATA LENGTH EXCEEDED
 * 26h/0Ch  DZTPRO    K    INVALID OPERATION FOR COPY SOURCE OR DESTINATION
 * 26h/0Dh  DZTPRO    K    COPY SEGMENT GRANULARITY VIOLATION
 * 26h/0Eh  DZTPROMAEBK    INVALID PARAMETER WHILE PORT IS ENABLED
 * 26h/0Fh              F  INVALID DATA-OUT BUFFER INTEGRITY CHECK VALUE
 * 26h/10h    T            DATA DECRYPTION KEY FAIL LIMIT REACHED
 * 26h/11h    T            INCOMPLETE KEY-ASSOCIATED DATA SET
 * 26h/12h    T            VENDOR SPECIFIC KEY REFERENCE NOT FOUND
 * 26h/13h  D              APPLICATION TAG MODE PAGE IS INVALID
 * 26h/14h    T            TAPE STREAM MIRRORING PREVENTED
 * 26h/15h    T            COPY SOURCE OR COPY DESTINATION NOT AUTHORIZED
 * 26h/16h  DZ             FAST COPY NOT POSSIBLE
 * 27h/00h  DZT RO   BK    WRITE PROTECTED
 * 27h/01h  DZT RO   BK    HARDWARE WRITE PROTECTED
 * 27h/02h  DZT RO   BK    LOGICAL UNIT SOFTWARE WRITE PROTECTED
 * 27h/03h    T R          ASSOCIATED WRITE PROTECT
 * 27h/04h    T R          PERSISTENT WRITE PROTECT
 * 27h/05h    T R          PERMANENT WRITE PROTECT
 * 27h/06h      R       F  CONDITIONAL WRITE PROTECT
 * 27h/07h  DZ       B     SPACE ALLOCATION FAILED WRITE PROTECT
 * 27h/08h  DZ             ZONE IS READ ONLY
 * 28h/00h  DZTPROMAEBKVF  NOT READY TO READY CHANGE, MEDIUM MAY HAVE CHANGED
 * 28h/01h  DZT ROM  B     IMPORT OR EXPORT ELEMENT ACCESSED
 * 28h/02h      R          FORMAT-LAYER MAY HAVE CHANGED
 * 28h/03h        M        IMPORT/EXPORT ELEMENT ACCESSED, MEDIUM CHANGED
 * 29h/00h  DZTPROMAEBKVF  POWER ON, RESET, OR BUS DEVICE RESET OCCURRED
 * 29h/01h  DZTPROMAEBKVF  POWER ON OCCURRED
 * 29h/02h  DZTPROMAEBKVF  SCSI BUS RESET OCCURRED
 * 29h/03h  DZTPROMAEBKVF  BUS DEVICE RESET FUNCTION OCCURRED
 * 29h/04h  DZTPROMAEBKVF  DEVICE INTERNAL RESET
 * 29h/05h  DZTPROMAEBKVF  TRANSCEIVER MODE CHANGED TO SINGLE-ENDED
 * 29h/06h  DZTPROMAEBKVF  TRANSCEIVER MODE CHANGED TO LVD
 * 29h/07h  DZTPROMAEBKVF  I_T NEXUS LOSS OCCURRED
 * 2Ah/00h  DZT ROMAEBKVF  PARAMETERS CHANGED
 * 2Ah/01h  DZT ROMAEBKVF  MODE PARAMETERS CHANGED
 * 2Ah/02h  DZT ROMAE K    LOG PARAMETERS CHANGED
 * 2Ah/03h  DZTPROMAE K    RESERVATIONS PREEMPTED
 * 2Ah/04h  DZTPROMAE      RESERVATIONS RELEASED
 * 2Ah/05h  DZTPROMAE      REGISTRATIONS PREEMPTED
 * 2Ah/06h  DZTPROMAEBKVF  ASYMMETRIC ACCESS STATE CHANGED
 * 2Ah/07h  DZTPROMAEBKVF  IMPLICIT ASYMMETRIC ACCESS STATE TRANSITION FAILED
 * 2Ah/08h  DZT ROMAEBKVF  PRIORITY CHANGED
 * 2Ah/09h  DZ             CAPACITY DATA HAS CHANGED
 * 2Ah/0Ah  DZT            ERROR HISTORY I_T NEXUS CLEARED
 * 2Ah/0Bh  DZT            ERROR HISTORY SNAPSHOT RELEASED
 * 2Ah/0Ch              F  ERROR RECOVERY ATTRIBUTES HAVE CHANGED
 * 2Ah/0Dh    T            DATA ENCRYPTION CAPABILITIES CHANGED
 * 2Ah/10h  DZT   M E  V   TIMESTAMP CHANGED
 * 2Ah/11h    T            DATA ENCRYPTION PARAMETERS CHANGED BY ANOTHER I_T NEXUS
 * 2Ah/12h    T            DATA ENCRYPTION PARAMETERS CHANGED BY VENDOR SPECIFIC EVENT
 * 2Ah/13h    T            DATA ENCRYPTION KEY INSTANCE COUNTER HAS CHANGED
 * 2Ah/14h  DZT R MAEBKV   SA CREATION CAPABILITIES DATA HAS CHANGED
 * 2Ah/15h    T   M    V   MEDIUM REMOVAL PREVENTION PREEMPTED
 * 2Ah/16h  DZ             ZONE RESET WRITE POINTER RECOMMENDED
 * 2Bh/00h  DZTPRO    K    COPY CANNOT EXECUTE SINCE HOST CANNOT DISCONNECT
 * 2Ch/00h  DZTPROMAEBKVF  COMMAND SEQUENCE ERROR
 * 2Ch/01h                 TOO MANY WINDOWS SPECIFIED
 * 2Ch/02h                 INVALID COMBINATION OF WINDOWS SPECIFIED
 * 2Ch/03h      R          CURRENT PROGRAM AREA IS NOT EMPTY
 * 2Ch/04h      R          CURRENT PROGRAM AREA IS EMPTY
 * 2Ch/05h           B     ILLEGAL POWER CONDITION REQUEST
 * 2Ch/06h      R          PERSISTENT PREVENT CONFLICT
 * 2Ch/07h  DZTPROMAEBKVF  PREVIOUS BUSY STATUS
 * 2Ch/08h  DZTPROMAEBKVF  PREVIOUS TASK SET FULL STATUS
 * 2Ch/09h  DZTPROM EBKVF  PREVIOUS RESERVATION CONFLICT STATUS
 * 2Ch/0Ah              F  PARTITION OR COLLECTION CONTAINS USER OBJECTS
 * 2Ch/0Bh    T            NOT RESERVED
 * 2Ch/0Ch  DZ             ORWRITE GENERATION DOES NOT MATCH
 * 2Ch/0Dh  DZ             RESET WRITE POINTER NOT ALLOWED
 * 2Ch/0Eh  DZ             ZONE IS OFFLINE
 * 2Ch/0Fh  DZ             STREAM NOT OPEN
 * 2Ch/10h  DZ             UNWRITTEN DATA IN ZONE
 * 2Ch/11h  D              DESCRIPTOR FORMAT SENSE DATA REQUIRED
 * 2Ch/12h  DZ             ZONE IS INACTIVE
 * 2Ch/13h  DZTPROMAEBKVF  WELL KNOWN LOGICAL UNIT ACCESS REQUIRED
 * 2Dh/00h    T            OVERWRITE ERROR ON UPDATE IN PLACE
 * 2Eh/00h  DZ  ROM  B     INSUFFICIENT TIME FOR OPERATION
 * 2Eh/01h  DZ   OM  B     COMMAND TIMEOUT BEFORE PROCESSING
 * 2Eh/02h  DZ   OM  B     COMMAND TIMEOUT DURING PROCESSING
 * 2Eh/03h  DZ   OM  B     COMMAND TIMEOUT DURING PROCESSING DUE TO ERROR RECOVERY
 * 2Fh/00h  DZTPROMAEBKVF  COMMANDS CLEARED BY ANOTHER INITIATOR
 * 2Fh/01h  DZ             COMMANDS CLEARED BY POWER LOSS NOTIFICATION
 * 2Fh/02h  DZTPROMAEBKVF  COMMANDS CLEARED BY DEVICE SERVER
 * 2Fh/03h  DZTPROMAEBKVF  SOME COMMANDS CLEARED BY QUEUING LAYER EVENT
 * 30h/00h  DZT ROM  BK    INCOMPATIBLE MEDIUM INSTALLED
 * 30h/01h  DZT RO   BK    CANNOT READ MEDIUM - UNKNOWN FORMAT
 * 30h/02h  DZT RO   BK    CANNOT READ MEDIUM - INCOMPATIBLE FORMAT
 * 30h/03h  DZT R M   K    CLEANING CARTRIDGE INSTALLED
 * 30h/04h  DZT RO   BK    CANNOT WRITE MEDIUM - UNKNOWN FORMAT
 * 30h/05h  DZT RO   BK    CANNOT WRITE MEDIUM - INCOMPATIBLE FORMAT
 * 30h/06h  DZT RO   B     CANNOT FORMAT MEDIUM - INCOMPATIBLE MEDIUM
 * 30h/07h  DZT ROMAEBKVF  CLEANING FAILURE
 * 30h/08h      R          CANNOT WRITE - APPLICATION CODE MISMATCH
 * 30h/09h      R          CURRENT SESSION NOT FIXATED FOR APPEND
 * 30h/0Ah  DZT RO AEBK    CLEANING REQUEST REJECTED
 * 30h/0Ch    T            WORM MEDIUM - OVERWRITE ATTEMPTED
 * 30h/0Dh    T            WORM MEDIUM - INTEGRITY CHECK
 * 30h/10h      R          MEDIUM NOT FORMATTED
 * 30h/11h        M        INCOMPATIBLE VOLUME TYPE
 * 30h/12h        M        INCOMPATIBLE VOLUME QUALIFIER
 * 30h/13h        M        CLEANING VOLUME EXPIRED
 * 31h/00h  DZT RO   BK    MEDIUM FORMAT CORRUPTED
 * 31h/01h  DZ  RO   B     FORMAT COMMAND FAILED
 * 31h/02h      R          ZONED FORMATTING FAILED DUE TO SPARE LINKING
 * 31h/03h  DZ       B     SANITIZE COMMAND FAILED
 * 31h/04h  DZ             DEPOPULATION FAILED
 * 31h/05h  DZ             DEPOPULATION RESTORATION FAILED
 * 32h/00h  DZ   O   BK    NO DEFECT SPARE LOCATION AVAILABLE
 * 32h/01h  DZ   O   BK    DEFECT LIST UPDATE FAILURE
 * 33h/00h    T            TAPE LENGTH ERROR
 * 34h/00h  DZTPROMAEBKVF  ENCLOSURE FAILURE
 * 35h/00h  DZTPROMAEBKVF  ENCLOSURE SERVICES FAILURE
 * 35h/01h  DZTPROMAEBKVF  UNSUPPORTED ENCLOSURE FUNCTION
 * 35h/02h  DZTPROMAEBKVF  ENCLOSURE SERVICES UNAVAILABLE
 * 35h/03h  DZTPROMAEBKVF  ENCLOSURE SERVICES TRANSFER FAILURE
 * 35h/04h  DZTPROMAEBKVF  ENCLOSURE SERVICES TRANSFER REFUSED
 * 35h/05h  DZT ROMAEBKVF  ENCLOSURE SERVICES CHECKSUM ERROR
 * 36h/00h                 RIBBON, INK, OR TONER FAILURE
 * 37h/00h  DZT ROMAEBKVF  ROUNDED PARAMETER
 * 38h/00h           B     EVENT STATUS NOTIFICATION
 * 38h/02h           B     ESN - POWER MANAGEMENT CLASS EVENT
 * 38h/04h           B     ESN - MEDIA CLASS EVENT
 * 38h/06h           B     ESN - DEVICE BUSY CLASS EVENT
 * 38h/07h  DZ             THIN PROVISIONING SOFT THRESHOLD REACHED
 * 38h/08h  DZ             DEPOPULATION INTERRUPTED
 * 39h/00h  DZT ROMAE K    SAVING PARAMETERS NOT SUPPORTED
 * 3Ah/00h  DZT ROM  BK    MEDIUM NOT PRESENT
 * 3Ah/01h  DZT ROM  BK    MEDIUM NOT PRESENT - TRAY CLOSED
 * 3Ah/02h  DZT ROM  BK    MEDIUM NOT PRESENT - TRAY OPEN
 * 3Ah/03h  DZT ROM  B     MEDIUM NOT PRESENT - LOADABLE
 * 3Ah/04h  DZT RO   B     MEDIUM NOT PRESENT - MEDIUM AUXILIARY MEMORY ACCESSIBLE
 * 3Bh/00h    T            SEQUENTIAL POSITIONING ERROR
 * 3Bh/01h    T            TAPE POSITION ERROR AT BEGINNING-OF-MEDIUM
 * 3Bh/02h    T            TAPE POSITION ERROR AT END-OF-MEDIUM
 * 3Bh/03h                 TAPE OR ELECTRONIC VERTICAL FORMS UNIT NOT READY
 * 3Bh/04h                 SLEW FAILURE
 * 3Bh/05h                 PAPER JAM
 * 3Bh/06h                 FAILED TO SENSE TOP-OF-FORM
 * 3Bh/07h                 FAILED TO SENSE BOTTOM-OF-FORM
 * 3Bh/08h    T            REPOSITION ERROR
 * 3Bh/09h                 READ PAST END OF MEDIUM
 * 3Bh/0Ah                 READ PAST BEGINNING OF MEDIUM
 * 3Bh/0Bh                 POSITION PAST END OF MEDIUM
 * 3Bh/0Ch    T            POSITION PAST BEGINNING OF MEDIUM
 * 3Bh/0Dh  DZT ROM  BK    MEDIUM DESTINATION ELEMENT FULL
 * 3Bh/0Eh  DZT ROM  BK    MEDIUM SOURCE ELEMENT EMPTY
 * 3Bh/0Fh      R          END OF MEDIUM REACHED
 * 3Bh/11h  DZT ROM  BK    MEDIUM MAGAZINE NOT ACCESSIBLE
 * 3Bh/12h  DZT ROM  BK    MEDIUM MAGAZINE REMOVED
 * 3Bh/13h  DZT ROM  BK    MEDIUM MAGAZINE INSERTED
 * 3Bh/14h  DZT ROM  BK    MEDIUM MAGAZINE LOCKED
 * 3Bh/15h  DZT ROM  BK    MEDIUM MAGAZINE UNLOCKED
 * 3Bh/16h      R          MECHANICAL POSITIONING OR CHANGER ERROR
 * 3Bh/17h              F  READ PAST END OF USER OBJECT
 * 3Bh/18h        M        ELEMENT DISABLED
 * 3Bh/19h        M        ELEMENT ENABLED
 * 3Bh/1Ah        M        DATA TRANSFER DEVICE REMOVED
 * 3Bh/1Bh        M        DATA TRANSFER DEVICE INSERTED
 * 3Bh/1Ch    T            TOO MANY LOGICAL OBJECTS ON PARTITION TO SUPPORT OPERATION
 * 3Bh/20h        M        ELEMENT STATIC INFORMATION CHANGED
 * 3Ch/00h
 * 3Dh/00h  DZTPROMAE K    INVALID BITS IN IDENTIFY MESSAGE
 * 3Eh/00h  DZTPROMAEBKVF  LOGICAL UNIT HAS NOT SELF-CONFIGURED YET
 * 3Eh/01h  DZTPROMAEBKVF  LOGICAL UNIT FAILURE
 * 3Eh/02h  DZTPROMAEBKVF  TIMEOUT ON LOGICAL UNIT
 * 3Eh/03h  DZTPROMAEBKVF  LOGICAL UNIT FAILED SELF-TEST
 * 3Eh/04h  DZTPROMAEBKVF  LOGICAL UNIT UNABLE TO UPDATE SELF-TEST LOG
 * 3Fh/00h  DZTPROMAEBKVF  TARGET OPERATING CONDITIONS HAVE CHANGED
 * 3Fh/01h  DZTPROMAEBKVF  MICROCODE HAS BEEN CHANGED
 * 3Fh/02h  DZTPROM  BK    CHANGED OPERATING DEFINITION
 * 3Fh/03h  DZTPROMAEBKVF  INQUIRY DATA HAS CHANGED
 * 3Fh/04h  DZT ROMAEBK    COMPONENT DEVICE ATTACHED
 * 3Fh/05h  DZT ROMAEBK    DEVICE IDENTIFIER CHANGED
 * 3Fh/06h  DZT ROMAEB     REDUNDANCY GROUP CREATED OR MODIFIED
 * 3Fh/07h  DZT ROMAEB     REDUNDANCY GROUP DELETED
 * 3Fh/08h  DZT ROMAEB     SPARE CREATED OR MODIFIED
 * 3Fh/09h  DZT ROMAEB     SPARE DELETED
 * 3Fh/0Ah  DZT ROMAEBK    VOLUME SET CREATED OR MODIFIED
 * 3Fh/0Bh  DZT ROMAEBK    VOLUME SET DELETED
 * 3Fh/0Ch  DZT ROMAEBK    VOLUME SET DEASSIGNED
 * 3Fh/0Dh  DZT ROMAEBK    VOLUME SET REASSIGNED
 * 3Fh/0Eh  DZTPROMAE      REPORTED LUNS DATA HAS CHANGED
 * 3Fh/0Fh  DZTPROMAEBKVF  ECHO BUFFER OVERWRITTEN
 * 3Fh/10h  DZT ROM  B     MEDIUM LOADABLE
 * 3Fh/11h  DZT ROM  B     MEDIUM AUXILIARY MEMORY ACCESSIBLE
 * 3Fh/12h  DZTPR MAEBK F  iSCSI IP ADDRESS ADDED
 * 3Fh/13h  DZTPR MAEBK F  iSCSI IP ADDRESS REMOVED
 * 3Fh/14h  DZTPR MAEBK F  iSCSI IP ADDRESS CHANGED
 * 3Fh/15h  DZTPR MAEBK    INSPECT REFERRALS SENSE DESCRIPTORS
 * 3Fh/16h  DZTPROMAEBKVF  MICROCODE HAS BEEN CHANGED WITHOUT RESET
 * 3Fh/17h  DZ             ZONE TRANSITION TO FULL
 * 3Fh/18h  D              BIND COMPLETED
 * 3Fh/19h  D              BIND REDIRECTED
 * 3Fh/1Ah  D              SUBSIDIARY BINDING CHANGED
 * 40h/00h  DZ             RAM FAILURE (SHOULD USE 40 NN)
 * 40h/NNh  DZTPROMAEBKVF  DIAGNOSTIC FAILURE ON COMPONENT NN (80h-FFh)
 * 41h/00h  DZ             DATA PATH FAILURE (SHOULD USE 40 NN)
 * 42h/00h  DZ             POWER-ON OR SELF-TEST FAILURE (SHOULD USE 40 NN)
 * 43h/00h  DZTPROMAEBKVF  MESSAGE ERROR
 * 44h/00h  DZTPROMAEBKVF  INTERNAL TARGET FAILURE
 * 44h/01h  DZTP  MAEBKVF  PERSISTENT RESERVATION INFORMATION LOST
 * 44h/71h  DZT      B     ATA DEVICE FAILED SET FEATURES
 * 45h/00h  DZTPROMAEBKVF  SELECT OR RESELECT FAILURE
 * 46h/00h  DZTPROM  BK    UNSUCCESSFUL SOFT RESET
 * 47h/00h  DZTPROMAEBKVF  SCSI PARITY ERROR
 * 47h/01h  DZTPROMAEBKVF  DATA PHASE CRC ERROR DETECTED
 * 47h/02h  DZTPROMAEBKVF  SCSI PARITY ERROR DETECTED DURING ST DATA PHASE
 * 47h/03h  DZTPROMAEBKVF  INFORMATION UNIT iuCRC ERROR DETECTED
 * 47h/04h  DZTPROMAEBKVF  ASYNCHRONOUS INFORMATION PROTECTION ERROR DETECTED
 * 47h/05h  DZTPROMAEBKVF  PROTOCOL SERVICE CRC ERROR
 * 47h/06h  DZT   MAEBKVF  PHY TEST FUNCTION IN PROGRESS
 * 47h/7Fh  DZTPROMAEBK    SOME COMMANDS CLEARED BY ISCSI PROTOCOL EVENT
 * 48h/00h  DZTPROMAEBKVF  INITIATOR DETECTED ERROR MESSAGE RECEIVED
 * 49h/00h  DZTPROMAEBKVF  INVALID MESSAGE ERROR
 * 4Ah/00h  DZTPROMAEBKVF  COMMAND PHASE ERROR
 * 4Bh/00h  DZTPROMAEBKVF  DATA PHASE ERROR
 * 4Bh/01h  DZTPROMAEBK    INVALID TARGET PORT TRANSFER TAG RECEIVED
 * 4Bh/02h  DZTPROMAEBK    TOO MUCH WRITE DATA
 * 4Bh/03h  DZTPROMAEBK    ACK/NAK TIMEOUT
 * 4Bh/04h  DZTPROMAEBK    NAK RECEIVED
 * 4Bh/05h  DZTPROMAEBK    DATA OFFSET ERROR
 * 4Bh/06h  DZTPROMAEBK    INITIATOR RESPONSE TIMEOUT
 * 4Bh/07h  DZTPROMAEBK F  CONNECTION LOST
 * 4Bh/08h  DZTPROMAEBK F  DATA-IN BUFFER OVERFLOW - DATA BUFFER SIZE
 * 4Bh/09h  DZTPROMAEBK F  DATA-IN BUFFER OVERFLOW - DATA BUFFER DESCRIPTOR AREA
 * 4Bh/0Ah  DZTPROMAEBK F  DATA-IN BUFFER ERROR
 * 4Bh/0Bh  DZTPROMAEBK F  DATA-OUT BUFFER OVERFLOW - DATA BUFFER SIZE
 * 4Bh/0Ch  DZTPROMAEBK F  DATA-OUT BUFFER OVERFLOW - DATA BUFFER DESCRIPTOR AREA
 * 4Bh/0Dh  DZTPROMAEBK F  DATA-OUT BUFFER ERROR
 * 4Bh/0Eh  DZTPROMAEBK F  PCIE FABRIC ERROR
 * 4Bh/0Fh  DZTPROMAEBK F  PCIE COMPLETION TIMEOUT
 * 4Bh/10h  DZTPROMAEBK F  PCIE COMPLETER ABORT
 * 4Bh/11h  DZTPROMAEBK F  PCIE POISONED TLP RECEIVED
 * 4Bh/12h  DZTPROMAEBK F  PCIE ECRC CHECK FAILED
 * 4Bh/13h  DZTPROMAEBK F  PCIE UNSUPPORTED REQUEST
 * 4Bh/14h  DZTPROMAEBK F  PCIE ACS VIOLATION
 * 4Bh/15h  DZTPROMAEBK F  PCIE TLP PREFIX BLOCKED
 * 4Ch/00h  DZTPROMAEBKVF  LOGICAL UNIT FAILED SELF-CONFIGURATION
 * 4Dh/NNh  DZTPROMAEBKVF  TAGGED OVERLAPPED COMMANDS (NN = TASK TAG)
 * 4Eh/00h  DZTPROMAEBKVF  OVERLAPPED COMMANDS ATTEMPTED
 * 4Fh/00h
 * 50h/00h    T            WRITE APPEND ERROR
 * 50h/01h    T            WRITE APPEND POSITION ERROR
 * 50h/02h    T            POSITION ERROR RELATED TO TIMING
 * 51h/00h  DZT RO         ERASE FAILURE
 * 51h/01h  DZ  R          ERASE FAILURE - INCOMPLETE ERASE OPERATION DETECTED
 * 52h/00h    T            CARTRIDGE FAULT
 * 53h/00h  DZT ROM  BK    MEDIA LOAD OR EJECT FAILED
 * 53h/01h    T            UNLOAD TAPE FAILURE
 * 53h/02h  DZT ROM  BK    MEDIUM REMOVAL PREVENTED
 * 53h/03h        M        MEDIUM REMOVAL PREVENTED BY DATA TRANSFER ELEMENT
 * 53h/04h    T            MEDIUM THREAD OR UNTHREAD FAILURE
 * 53h/05h        M        VOLUME IDENTIFIER INVALID
 * 53h/06h        M        VOLUME IDENTIFIER MISSING
 * 53h/07h        M        DUPLICATE VOLUME IDENTIFIER
 * 53h/08h        M        ELEMENT STATUS UNKNOWN
 * 53h/09h        M        DATA TRANSFER DEVICE ERROR - LOAD FAILED
 * 53h/0Ah        M        DATA TRANSFER DEVICE ERROR - UNLOAD FAILED
 * 53h/0Bh        M        DATA TRANSFER DEVICE ERROR - UNLOAD MISSING
 * 53h/0Ch        M        DATA TRANSFER DEVICE ERROR - EJECT FAILED
 * 53h/0Dh        M        DATA TRANSFER DEVICE ERROR - LIBRARY COMMUNICATION FAILED
 * 54h/00h     P           SCSI TO HOST SYSTEM INTERFACE FAILURE
 * 55h/00h     P           SYSTEM RESOURCE FAILURE
 * 55h/01h  DZ   O   BK    SYSTEM BUFFER FULL
 * 55h/02h  DZTPROMAE K    INSUFFICIENT RESERVATION RESOURCES
 * 55h/03h  DZTPROMAE K    INSUFFICIENT RESOURCES
 * 55h/04h  DZTPROMAE K    INSUFFICIENT REGISTRATION RESOURCES
 * 55h/05h  DZTPROMAEBK    INSUFFICIENT ACCESS CONTROL RESOURCES
 * 55h/06h  DZT ROM  B     AUXILIARY MEMORY OUT OF SPACE
 * 55h/07h              F  QUOTA ERROR
 * 55h/08h    T            MAXIMUM NUMBER OF SUPPLEMENTAL DECRYPTION KEYS EXCEEDED
 * 55h/09h        M        MEDIUM AUXILIARY MEMORY NOT ACCESSIBLE
 * 55h/0Ah  DZ    M        DATA CURRENTLY UNAVAILABLE
 * 55h/0Bh  DZTPROMAEBKVF  INSUFFICIENT POWER FOR OPERATION
 * 55h/0Ch  DZTP     B     INSUFFICIENT RESOURCES TO CREATE ROD
 * 55h/0Dh  DZTP     B     INSUFFICIENT RESOURCES TO CREATE ROD TOKEN
 * 55h/0Eh  DZ             INSUFFICIENT ZONE RESOURCES
 * 55h/0Fh  DZ             INSUFFICIENT ZONE RESOURCES TO COMPLETE WRITE
 * 55h/10h  DZ             MAXIMUM NUMBER OF STREAMS OPEN
 * 55h/11h  D              INSUFFICIENT RESOURCES TO BIND
 * 56h/00h
 * 57h/00h      R          UNABLE TO RECOVER TABLE-OF-CONTENTS
 * 58h/00h       O         GENERATION DOES NOT EXIST
 * 59h/00h       O         UPDATED BLOCK READ
 * 5Ah/00h  DZTPRO   BK    OPERATOR REQUEST OR STATE CHANGE INPUT
 * 5Ah/01h  DZT ROM  BK    OPERATOR MEDIUM REMOVAL REQUEST
 * 5Ah/02h  DZT RO A BK    OPERATOR SELECTED WRITE PROTECT
 * 5Ah/03h  DZT RO A BK    OPERATOR SELECTED WRITE PERMIT
 * 5Bh/00h  DZTPROM   K    LOG EXCEPTION
 * 5Bh/01h  DZTPROM   K    THRESHOLD CONDITION MET
 * 5Bh/02h  DZTPROM   K    LOG COUNTER AT MAXIMUM
 * 5Bh/03h  DZTPROM   K    LOG LIST CODES EXHAUSTED
 * 5Ch/00h  DZ   O         RPL STATUS CHANGE
 * 5Ch/01h  DZ   O         SPINDLES SYNCHRONIZED
 * 5Ch/02h  DZ   O         SPINDLES NOT SYNCHRONIZED
 * 5Dh/00h  DZTPROMAEBKVF  FAILURE PREDICTION THRESHOLD EXCEEDED
 * 5Dh/01h      R    B     MEDIA FAILURE PREDICTION THRESHOLD EXCEEDED
 * 5Dh/02h      R          LOGICAL UNIT FAILURE PREDICTION THRESHOLD EXCEEDED
 * 5Dh/03h      R          SPARE AREA EXHAUSTION PREDICTION THRESHOLD EXCEEDED
 * 5Dh/10h  DZ       B     HARDWARE IMPENDING FAILURE GENERAL HARD DRIVE FAILURE
 * 5Dh/11h  DZ       B     HARDWARE IMPENDING FAILURE DRIVE ERROR RATE TOO HIGH
 * 5Dh/12h  DZ       B     HARDWARE IMPENDING FAILURE DATA ERROR RATE TOO HIGH
 * 5Dh/13h  DZ       B     HARDWARE IMPENDING FAILURE SEEK ERROR RATE TOO HIGH
 * 5Dh/14h  DZ       B     HARDWARE IMPENDING FAILURE TOO MANY BLOCK REASSIGNS
 * 5Dh/15h  DZ       B     HARDWARE IMPENDING FAILURE ACCESS TIMES TOO HIGH
 * 5Dh/16h  DZ       B     HARDWARE IMPENDING FAILURE START UNIT TIMES TOO HIGH
 * 5Dh/17h  DZ       B     HARDWARE IMPENDING FAILURE CHANNEL PARAMETRICS
 * 5Dh/18h  DZ       B     HARDWARE IMPENDING FAILURE CONTROLLER DETECTED
 * 5Dh/19h  DZ       B     HARDWARE IMPENDING FAILURE THROUGHPUT PERFORMANCE
 * 5Dh/1Ah  DZ       B     HARDWARE IMPENDING FAILURE SEEK TIME PERFORMANCE
 * 5Dh/1Bh  DZ       B     HARDWARE IMPENDING FAILURE SPIN-UP RETRY COUNT
 * 5Dh/1Ch  DZ       B     HARDWARE IMPENDING FAILURE DRIVE CALIBRATION RETRY COUNT
 * 5Dh/1Dh  DZ       B     HARDWARE IMPENDING FAILURE POWER LOSS PROTECTION CIRCUIT
 * 5Dh/20h  DZ       B     CONTROLLER IMPENDING FAILURE GENERAL HARD DRIVE FAILURE
 * 5Dh/21h  DZ       B     CONTROLLER IMPENDING FAILURE DRIVE ERROR RATE TOO HIGH
 * 5Dh/22h  DZ       B     CONTROLLER IMPENDING FAILURE DATA ERROR RATE TOO HIGH
 * 5Dh/23h  DZ       B     CONTROLLER IMPENDING FAILURE SEEK ERROR RATE TOO HIGH
 * 5Dh/24h  DZ       B     CONTROLLER IMPENDING FAILURE TOO MANY BLOCK REASSIGNS
 * 5Dh/25h  DZ       B     CONTROLLER IMPENDING FAILURE ACCESS TIMES TOO HIGH
 * 5Dh/26h  DZ       B     CONTROLLER IMPENDING FAILURE START UNIT TIMES TOO HIGH
 * 5Dh/27h  DZ       B     CONTROLLER IMPENDING FAILURE CHANNEL PARAMETRICS
 * 5Dh/28h  DZ       B     CONTROLLER IMPENDING FAILURE CONTROLLER DETECTED
 * 5Dh/29h  DZ       B     CONTROLLER IMPENDING FAILURE THROUGHPUT PERFORMANCE
 * 5Dh/2Ah  DZ       B     CONTROLLER IMPENDING FAILURE SEEK TIME PERFORMANCE
 * 5Dh/2Bh  DZ       B     CONTROLLER IMPENDING FAILURE SPIN-UP RETRY COUNT
 * 5Dh/2Ch  DZ       B     CONTROLLER IMPENDING FAILURE DRIVE CALIBRATION RETRY COUNT
 * 5Dh/30h  DZ       B     DATA CHANNEL IMPENDING FAILURE GENERAL HARD DRIVE FAILURE
 * 5Dh/31h  DZ       B     DATA CHANNEL IMPENDING FAILURE DRIVE ERROR RATE TOO HIGH
 * 5Dh/32h  DZ       B     DATA CHANNEL IMPENDING FAILURE DATA ERROR RATE TOO HIGH
 * 5Dh/33h  DZ       B     DATA CHANNEL IMPENDING FAILURE SEEK ERROR RATE TOO HIGH
 * 5Dh/34h  DZ       B     DATA CHANNEL IMPENDING FAILURE TOO MANY BLOCK REASSIGNS
 * 5Dh/35h  DZ       B     DATA CHANNEL IMPENDING FAILURE ACCESS TIMES TOO HIGH
 * 5Dh/36h  DZ       B     DATA CHANNEL IMPENDING FAILURE START UNIT TIMES TOO HIGH
 * 5Dh/37h  DZ       B     DATA CHANNEL IMPENDING FAILURE CHANNEL PARAMETRICS
 * 5Dh/38h  DZ       B     DATA CHANNEL IMPENDING FAILURE CONTROLLER DETECTED
 * 5Dh/39h  DZ       B     DATA CHANNEL IMPENDING FAILURE THROUGHPUT PERFORMANCE
 * 5Dh/3Ah  DZ       B     DATA CHANNEL IMPENDING FAILURE SEEK TIME PERFORMANCE
 * 5Dh/3Bh  DZ       B     DATA CHANNEL IMPENDING FAILURE SPIN-UP RETRY COUNT
 * 5Dh/3Ch  DZ       B     DATA CHANNEL IMPENDING FAILURE DRIVE CALIBRATION RETRY COUNT
 * 5Dh/40h  DZ       B     SERVO IMPENDING FAILURE GENERAL HARD DRIVE FAILURE
 * 5Dh/41h  DZ       B     SERVO IMPENDING FAILURE DRIVE ERROR RATE TOO HIGH
 * 5Dh/42h  DZ       B     SERVO IMPENDING FAILURE DATA ERROR RATE TOO HIGH
 * 5Dh/43h  DZ       B     SERVO IMPENDING FAILURE SEEK ERROR RATE TOO HIGH
 * 5Dh/44h  DZ       B     SERVO IMPENDING FAILURE TOO MANY BLOCK REASSIGNS
 * 5Dh/45h  DZ       B     SERVO IMPENDING FAILURE ACCESS TIMES TOO HIGH
 * 5Dh/46h  DZ       B     SERVO IMPENDING FAILURE START UNIT TIMES TOO HIGH
 * 5Dh/47h  DZ       B     SERVO IMPENDING FAILURE CHANNEL PARAMETRICS
 * 5Dh/48h  DZ       B     SERVO IMPENDING FAILURE CONTROLLER DETECTED
 * 5Dh/49h  DZ       B     SERVO IMPENDING FAILURE THROUGHPUT PERFORMANCE
 * 5Dh/4Ah  DZ       B     SERVO IMPENDING FAILURE SEEK TIME PERFORMANCE
 * 5Dh/4Bh  DZ       B     SERVO IMPENDING FAILURE SPIN-UP RETRY COUNT
 * 5Dh/4Ch  DZ       B     SERVO IMPENDING FAILURE DRIVE CALIBRATION RETRY COUNT
 * 5Dh/50h  DZ       B     SPINDLE IMPENDING FAILURE GENERAL HARD DRIVE FAILURE
 * 5Dh/51h  DZ       B     SPINDLE IMPENDING FAILURE DRIVE ERROR RATE TOO HIGH
 * 5Dh/52h  DZ       B     SPINDLE IMPENDING FAILURE DATA ERROR RATE TOO HIGH
 * 5Dh/53h  DZ       B     SPINDLE IMPENDING FAILURE SEEK ERROR RATE TOO HIGH
 * 5Dh/54h  DZ       B     SPINDLE IMPENDING FAILURE TOO MANY BLOCK REASSIGNS
 * 5Dh/55h  DZ       B     SPINDLE IMPENDING FAILURE ACCESS TIMES TOO HIGH
 * 5Dh/56h  DZ       B     SPINDLE IMPENDING FAILURE START UNIT TIMES TOO HIGH
 * 5Dh/57h  DZ       B     SPINDLE IMPENDING FAILURE CHANNEL PARAMETRICS
 * 5Dh/58h  DZ       B     SPINDLE IMPENDING FAILURE CONTROLLER DETECTED
 * 5Dh/59h  DZ       B     SPINDLE IMPENDING FAILURE THROUGHPUT PERFORMANCE
 * 5Dh/5Ah  DZ       B     SPINDLE IMPENDING FAILURE SEEK TIME PERFORMANCE
 * 5Dh/5Bh  DZ       B     SPINDLE IMPENDING FAILURE SPIN-UP RETRY COUNT
 * 5Dh/5Ch  DZ       B     SPINDLE IMPENDING FAILURE DRIVE CALIBRATION RETRY COUNT
 * 5Dh/60h  DZ       B     FIRMWARE IMPENDING FAILURE GENERAL HARD DRIVE FAILURE
 * 5Dh/61h  DZ       B     FIRMWARE IMPENDING FAILURE DRIVE ERROR RATE TOO HIGH
 * 5Dh/62h  DZ       B     FIRMWARE IMPENDING FAILURE DATA ERROR RATE TOO HIGH
 * 5Dh/63h  DZ       B     FIRMWARE IMPENDING FAILURE SEEK ERROR RATE TOO HIGH
 * 5Dh/64h  DZ       B     FIRMWARE IMPENDING FAILURE TOO MANY BLOCK REASSIGNS
 * 5Dh/65h  DZ       B     FIRMWARE IMPENDING FAILURE ACCESS TIMES TOO HIGH
 * 5Dh/66h  DZ       B     FIRMWARE IMPENDING FAILURE START UNIT TIMES TOO HIGH
 * 5Dh/67h  DZ       B     FIRMWARE IMPENDING FAILURE CHANNEL PARAMETRICS
 * 5Dh/68h  DZ       B     FIRMWARE IMPENDING FAILURE CONTROLLER DETECTED
 * 5Dh/69h  DZ       B     FIRMWARE IMPENDING FAILURE THROUGHPUT PERFORMANCE
 * 5Dh/6Ah  DZ       B     FIRMWARE IMPENDING FAILURE SEEK TIME PERFORMANCE
 * 5Dh/6Bh  DZ       B     FIRMWARE IMPENDING FAILURE SPIN-UP RETRY COUNT
 * 5Dh/6Ch  DZ       B     FIRMWARE IMPENDING FAILURE DRIVE CALIBRATION RETRY COUNT
 * 5Dh/73h  DZ       B     MEDIA IMPENDING FAILURE ENDURANCE LIMIT MET
 * 5Dh/FFh  DZTPROMAEBKVF  FAILURE PREDICTION THRESHOLD EXCEEDED (FALSE)
 * 5Eh/00h  DZTPRO A  K    LOW POWER CONDITION ON
 * 5Eh/01h  DZTPRO A  K    IDLE CONDITION ACTIVATED BY TIMER
 * 5Eh/02h  DZTPRO A  K    STANDBY CONDITION ACTIVATED BY TIMER
 * 5Eh/03h  DZTPRO A  K    IDLE CONDITION ACTIVATED BY COMMAND
 * 5Eh/04h  DZTPRO A  K    STANDBY CONDITION ACTIVATED BY COMMAND
 * 5Eh/05h  DZTPRO A  K    IDLE_B CONDITION ACTIVATED BY TIMER
 * 5Eh/06h  DZTPRO A  K    IDLE_B CONDITION ACTIVATED BY COMMAND
 * 5Eh/07h  DZTPRO A  K    IDLE_C CONDITION ACTIVATED BY TIMER
 * 5Eh/08h  DZTPRO A  K    IDLE_C CONDITION ACTIVATED BY COMMAND
 * 5Eh/09h  DZTPRO A  K    STANDBY_Y CONDITION ACTIVATED BY TIMER
 * 5Eh/0Ah  DZTPRO A  K    STANDBY_Y CONDITION ACTIVATED BY COMMAND
 * 5Eh/41h           B     POWER STATE CHANGE TO ACTIVE
 * 5Eh/42h           B     POWER STATE CHANGE TO IDLE
 * 5Eh/43h           B     POWER STATE CHANGE TO STANDBY
 * 5Eh/45h           B     POWER STATE CHANGE TO SLEEP
 * 5Eh/47h           BK    POWER STATE CHANGE TO DEVICE CONTROL
 * 5Fh/00h
 * 60h/00h                 LAMP FAILURE
 * 61h/00h                 VIDEO ACQUISITION ERROR
 * 61h/01h                 UNABLE TO ACQUIRE VIDEO
 * 61h/02h                 OUT OF FOCUS
 * 62h/00h                 SCAN HEAD POSITIONING ERROR
 * 63h/00h      R          END OF USER AREA ENCOUNTERED ON THIS TRACK
 * 63h/01h      R          PACKET DOES NOT FIT IN AVAILABLE SPACE
 * 64h/00h      R          ILLEGAL MODE FOR THIS TRACK
 * 64h/01h      R          INVALID PACKET SIZE
 * 65h/00h  DZTPROMAEBKVF  VOLTAGE FAULT
 * 66h/00h                 AUTOMATIC DOCUMENT FEEDER COVER UP
 * 66h/01h                 AUTOMATIC DOCUMENT FEEDER LIFT UP
 * 66h/02h                 DOCUMENT JAM IN AUTOMATIC DOCUMENT FEEDER
 * 66h/03h                 DOCUMENT MISS FEED AUTOMATIC IN DOCUMENT FEEDER
 * 67h/00h         A       CONFIGURATION FAILURE
 * 67h/01h         A       CONFIGURATION OF INCAPABLE LOGICAL UNITS FAILED
 * 67h/02h         A       ADD LOGICAL UNIT FAILED
 * 67h/03h         A       MODIFICATION OF LOGICAL UNIT FAILED
 * 67h/04h         A       EXCHANGE OF LOGICAL UNIT FAILED
 * 67h/05h         A       REMOVE OF LOGICAL UNIT FAILED
 * 67h/06h         A       ATTACHMENT OF LOGICAL UNIT FAILED
 * 67h/07h         A       CREATION OF LOGICAL UNIT FAILED
 * 67h/08h         A       ASSIGN FAILURE OCCURRED
 * 67h/09h         A       MULTIPLY ASSIGNED LOGICAL UNIT
 * 67h/0Ah  DZTPROMAEBKVF  SET TARGET PORT GROUPS COMMAND FAILED
 * 67h/0Bh  DZT      B     ATA DEVICE FEATURE NOT ENABLED
 * 67h/0Ch  D              COMMAND REJECTED
 * 67h/0Dh  D              EXPLICIT BIND NOT ALLOWED
 * 68h/00h         A       LOGICAL UNIT NOT CONFIGURED
 * 68h/01h  DZ             SUBSIDIARY LOGICAL UNIT NOT CONFIGURED
 * 69h/00h         A       DATA LOSS ON LOGICAL UNIT
 * 69h/01h         A       MULTIPLE LOGICAL UNIT FAILURES
 * 69h/02h         A       PARITY/DATA MISMATCH
 * 6Ah/00h         A       INFORMATIONAL, REFER TO LOG
 * 6Bh/00h         A       STATE CHANGE HAS OCCURRED
 * 6Bh/01h         A       REDUNDANCY LEVEL GOT BETTER
 * 6Bh/02h         A       REDUNDANCY LEVEL GOT WORSE
 * 6Ch/00h         A       REBUILD FAILURE OCCURRED
 * 6Dh/00h         A       RECALCULATE FAILURE OCCURRED
 * 6Eh/00h         A       COMMAND TO LOGICAL UNIT FAILED
 * 6Fh/00h      R          COPY PROTECTION KEY EXCHANGE FAILURE - AUTHENTICATION FAILURE
 * 6Fh/01h      R          COPY PROTECTION KEY EXCHANGE FAILURE - KEY NOT PRESENT
 * 6Fh/02h      R          COPY PROTECTION KEY EXCHANGE FAILURE - KEY NOT ESTABLISHED
 * 6Fh/03h      R          READ OF SCRAMBLED SECTOR WITHOUT AUTHENTICATION
 * 6Fh/04h      R          MEDIA REGION CODE IS MISMATCHED TO LOGICAL UNIT REGION
 * 6Fh/05h      R          DRIVE REGION MUST BE PERMANENT/REGION RESET COUNT ERROR
 * 6Fh/06h      R          INSUFFICIENT BLOCK COUNT FOR BINDING NONCE RECORDING
 * 6Fh/07h      R          CONFLICT IN BINDING NONCE RECORDING
 * 6Fh/08h      R          INSUFFICIENT PERMISSION
 * 6Fh/09h      R          INVALID DRIVE-HOST PAIRING SERVER
 * 6Fh/0Ah      R          DRIVE-HOST PAIRING SUSPENDED
 * 70h/NNh    T            DECOMPRESSION EXCEPTION SHORT ALGORITHM ID OF NN
 * 71h/00h    T            DECOMPRESSION EXCEPTION LONG ALGORITHM ID
 * 72h/00h      R          SESSION FIXATION ERROR
 * 72h/01h      R          SESSION FIXATION ERROR WRITING LEAD-IN
 * 72h/02h      R          SESSION FIXATION ERROR WRITING LEAD-OUT
 * 72h/03h      R          SESSION FIXATION ERROR - INCOMPLETE TRACK IN SESSION
 * 72h/04h      R          EMPTY OR PARTIALLY WRITTEN RESERVED TRACK
 * 72h/05h      R          NO MORE TRACK RESERVATIONS ALLOWED
 * 72h/06h      R          RMZ EXTENSION IS NOT ALLOWED
 * 72h/07h      R          NO MORE TEST ZONE EXTENSIONS ARE ALLOWED
 * 73h/00h      R          CD CONTROL ERROR
 * 73h/01h      R          POWER CALIBRATION AREA ALMOST FULL
 * 73h/02h      R          POWER CALIBRATION AREA IS FULL
 * 73h/03h      R          POWER CALIBRATION AREA ERROR
 * 73h/04h      R          PROGRAM MEMORY AREA UPDATE FAILURE
 * 73h/05h      R          PROGRAM MEMORY AREA IS FULL
 * 73h/06h      R          RMA/PMA IS ALMOST FULL
 * 73h/10h      R          CURRENT POWER CALIBRATION AREA ALMOST FULL
 * 73h/11h      R          CURRENT POWER CALIBRATION AREA IS FULL
 * 73h/17h      R          RDZ IS FULL
 * 74h/00h    T            SECURITY ERROR
 * 74h/01h    T            UNABLE TO DECRYPT DATA
 * 74h/02h    T            UNENCRYPTED DATA ENCOUNTERED WHILE DECRYPTING
 * 74h/03h    T            INCORRECT DATA ENCRYPTION KEY
 * 74h/04h    T            CRYPTOGRAPHIC INTEGRITY VALIDATION FAILED
 * 74h/05h    T            ERROR DECRYPTING DATA
 * 74h/06h    T            UNKNOWN SIGNATURE VERIFICATION KEY
 * 74h/07h    T            ENCRYPTION PARAMETERS NOT USEABLE
 * 74h/08h  DZT R M E  VF  DIGITAL SIGNATURE VALIDATION FAILURE
 * 74h/09h    T            ENCRYPTION MODE MISMATCH ON READ
 * 74h/0Ah    T            ENCRYPTED BLOCK NOT RAW READ ENABLED
 * 74h/0Bh    T            INCORRECT ENCRYPTION PARAMETERS
 * 74h/0Ch  DZT R MAEBKV   UNABLE TO DECRYPT PARAMETER LIST
 * 74h/0Dh    T            ENCRYPTION ALGORITHM DISABLED
 * 74h/10h  DZT R MAEBKV   SA CREATION PARAMETER VALUE INVALID
 * 74h/11h  DZT R MAEBKV   SA CREATION PARAMETER VALUE REJECTED
 * 74h/12h  DZT R MAEBKV   INVALID SA USAGE
 * 74h/21h    T            DATA ENCRYPTION CONFIGURATION PREVENTED
 * 74h/30h  DZT R MAEBKV   SA CREATION PARAMETER NOT SUPPORTED
 * 74h/40h  DZT R MAEBKV   AUTHENTICATION FAILED
 * 74h/61h             V   EXTERNAL DATA ENCRYPTION KEY MANAGER ACCESS ERROR
 * 74h/62h             V   EXTERNAL DATA ENCRYPTION KEY MANAGER ERROR
 * 74h/63h             V   EXTERNAL DATA ENCRYPTION KEY NOT FOUND
 * 74h/64h             V   EXTERNAL DATA ENCRYPTION REQUEST NOT AUTHORIZED
 * 74h/6Eh    T            EXTERNAL DATA ENCRYPTION CONTROL TIMEOUT
 * 74h/6Fh    T            EXTERNAL DATA ENCRYPTION CONTROL ERROR
 * 74h/71h  DZT R M E  V   LOGICAL UNIT ACCESS NOT AUTHORIZED
 * 74h/79h  DZ             SECURITY CONFLICT IN TRANSLATED DEVICE
*/

static void
decode_scsi_sense(const char *indent, struct scsi_sense_data *ssd)
{
    uint key = SSD_SENSE_KEY(ssd->flags);
    printf("%s  response=%02x valid=%d\n", indent, ssd->response_code,
           ssd->response_code & SSD_RCODE_VALID ? 1 : 0);

    if (ssd->response_code & SSD_RCODE_VALID) {
        int count;
        int len;
        printf("%s  seg=%x key=%02x %s\n",
               indent, ssd->segment, key, decode_scsi_sense_key(key));
        printf("%s  ili=%02x eom=%02x fmark=%02x\n", indent,
               ssd->flags & SSD_ILI ? 1 : 0,
               ssd->flags & SSD_EOM ? 1 : 0,
               ssd->flags & SSD_FILEMARK ? 1 : 0);
        printf("%s  info %02x %02x %02x %02x extra_len=0x%x\n", indent,
               ssd->info[0], ssd->info[1], ssd->info[2], ssd->info[3],
               ssd->extra_len);
        len = SSD_ADD_BYTES_LIM(ssd);
        if (len > 0) {
            printf("%s  extra ", indent);
            for (count = 0; count < len; count++)
                printf(" 0x%x", ssd->csi[count]);
            printf("\n");
        }
    }
}

static void
print_xs(struct scsipi_xfer *xs, int indent_count)
{
    char indent[32];
    sprintf(indent, "%*s", indent_count, "");
    if (xs->xs_callout.func == NULL) {
        printf("%sxs_callout  NONE\n", indent);
    } else {
        printf("%sxs_callout ticks=%d\n", indent, xs->xs_callout.ticks);
        printf("%s           func=%p(%p)\n",
               indent, xs->xs_callout.func, xs->xs_callout.arg);
    }
    printf("%sxs_done_cb=%p(%p)\n",
           indent, xs->xs_done_callback, xs->xs_callback_arg);
    printf("%samiga_ior=%p\n", indent, xs->amiga_ior);
    struct IOStdReq *ior = xs->amiga_ior;
    if (ior != NULL) {
        printf("%s  io_Device=%p\n", indent, ior->io_Device);
        printf("%s  io_Unit=%p\n", indent, ior->io_Unit);
        printf("%s  io_Command=%04x %s\n", indent,
               ior->io_Command, iocmd_name(ior->io_Command));
        decode_io_command(indent_count + 4, ior);
        printf("%s  io_Flags=%02x\n", indent, ior->io_Flags);
        printf("%s  io_Error=%02x\n", indent, ior->io_Error);
        printf("%s  io_Actual=%08x\n", indent, (uint) ior->io_Actual);
        printf("%s  io_Offset=%08x\n", indent, (uint) ior->io_Offset);
        printf("%s  io_Length=%08x\n", indent, (uint) ior->io_Length);
        printf("%s  io_Data=%p\n", indent, ior->io_Data);
    }
    printf("%sxs_control=%08x  ", indent, xs->xs_control);
    print_bits(bits_xs_control, ARRAY_SIZE(bits_xs_control),
               xs->xs_control);
    printf("\n");
    printf("%sxs_status=%d %s\n", indent, xs->xs_status,
           (xs->xs_status & XS_STS_DONE) ? "STS_DONE" : "");
    printf("%sxs_periph=%p", indent, xs->xs_periph);
    if (xs->xs_periph != NULL) {
        printf(" %d.%d",
               xs->xs_periph->periph_target, xs->xs_periph->periph_lun);
    }
    printf("\n");
    printf("%sxs_retries=%d\n", indent, xs->xs_retries);
    printf("%sxs_requeuecnt=%d\n", indent, xs->xs_requeuecnt);
    printf("%stimeout=%d\n", indent, xs->timeout);
    printf("%scmd=%p cmdlen=%d\n", indent, xs->cmd, xs->cmdlen);
    struct scsipi_generic *cmd = xs->cmd;
    if ((cmd != NULL) && (xs->cmdlen != 0)) {
        decode_scsi_command(indent, (uint8_t *)cmd, xs->cmdlen);
    }
    printf("%sdata=%p datalen=%d\n", indent, xs->data, xs->datalen);
    printf("%sresid=%d\n", indent, xs->resid);
    printf("%serror=%d %s\n",
           indent, (int) xs->error, decode_xs_error(xs->error));
    printf("%sbp=%p\n", indent, xs->bp);
    printf("%sscsi_sense=%p\n", indent, &xs->sense);
    decode_scsi_sense(indent, &xs->sense.scsi_sense);
    printf("%sstatus=%d %s\n",
           indent, (int) xs->status, decode_xs_status(xs->status));
    printf("%sxs_tag_type=%u xs_tag_id=%u\n",
           indent, xs->xs_tag_type, xs->xs_tag_id);
    printf("%scmdstore=%p\n", indent, &xs->cmdstore);
}

static void
decode_acb(struct siop_acb *acb)
{
    if (acb == NULL) {
        printf(" NULL acb\n");
        return;
    }
    if (acb->xs == NULL) {
        printf(" NULL xs");
    } else {
        if (acb->xs->xs_periph == NULL) {
            printf(" %p <NO PERIPH>", acb->xs);
        } else {
            struct scsipi_xfer *xs = acb->xs;
            printf(" %p %d.%d", xs,
                   xs->xs_periph->periph_target, xs->xs_periph->periph_lun);
            if (xs->xs_callout.func != NULL)
                printf(" [%d ticks]", xs->xs_callout.ticks);
        }
    }
    printf(" flags=%x len=%02d", acb->flags, acb->clen);
    decode_scsi_command("", (uint8_t *) &acb->cmd, acb->clen);
}

static void
show_sc_list(int indent_count, struct siop_acb *acb)
{
    char indent[32];
    int  newline = -1;
    sprintf(indent, "%*s", indent_count, "");

    if (acb == NULL) {
        printf("NONE\n");
        return;
    }
    while (acb != NULL) {
        if (newline > 0)
            printf("%s", indent);
        if (acb->xs != NULL) {
            if (newline == 0)
                printf("\n%s", indent);
            printf("%p", acb);
            decode_acb(acb);
            newline = 1;
        } else {
            printf("%p ", acb);
            newline = 0;
        }
        acb = acb->chain.tqe_next;
    }
    if (newline == 0)
        printf("\n");
}

static const char * const scsi_periph_type_name[] = {
    "T_DIRECT - direct access device",                     // 0x00
    "T_SEQUENTIAL - sequential access device",             // 0x01
    "T_PRINTER - printer device",                          // 0x02
    "T_PROCESSOR - processor device",                      // 0x03
    "T_WORM - write once, read many device",               // 0x04
    "T_CDROM - cd-rom device",                             // 0x05
    "T_SCANNER - scanner device",                          // 0x06
    "T_OPTICAL - optical memory device",                   // 0x07
    "T_CHANGER - medium changer device",                   // 0x08
    "T_COMM - communication device",                       // 0x09
    "T_IT8_1 - Defined by ASC IT8",                        // 0x0a
    "T_IT8_2 - Graphic arts pre-press device",             // 0x0b
    "T_STORARRAY - storage array device",                  // 0x0c
    "T_ENCLOSURE - enclosure services device",             // 0x0d
    "T_SIMPLE_DIRECT - Simplified direct-access device",   // 0x0e
    "T_OPTIC_CARD_RW - Optical card reader/writer device", // 0x0f
    "",                                                    // 0x10
    "T_OBJECT_STORED - Object-based Storage Device",       // 0x11
    "T_AUTOMATION_DRIVE - Automation drive interface",     // 0x12
    "",                                                    // 0x13
    "",                                                    // 0x13
    "",                                                    // 0x14
    "",                                                    // 0x15
    "",                                                    // 0x16
    "",                                                    // 0x17
    "",                                                    // 0x18
    "",                                                    // 0x19
    "",                                                    // 0x1a
    "",                                                    // 0x1b
    "",                                                    // 0x1c
    "",                                                    // 0x1d
    "T_WELL_KNOWN_LUN - Well known logical unit",          // 0x1e
    "T_NODEVICE - Unknown or no device type",              // 0x1f
};

static const char * const interrupt_type_name[] = {
    "NT_UNKNOWN",      // 0
    "NT_TASK",         // 1
    "NT_INTERRUPT",    // 2
    "NT_DEVICE",       // 3
    "NT_MSGPORT",      // 4
    "NT_MESSAGE",      // 5
    "NT_FREEMSG",      // 6
    "NT_REPLYMSG",     // 7
    "NT_RESOURCE",     // 8
    "NT_LIBRARY",      // 9
    "NT_MEMORY",       // 10
    "NT_SOFTINT",      // 11
    "NT_FONT",         // 12
    "NT_PROCESS",      // 13
    "NT_SEMAPHORE",    // 14
    "NT_SIGNALSEM",    // 15
    "NT_BOOTNODE",     // 16
    "NT_KICKMEM",      // 17
    "NT_GRAPHICS",     // 18
    "NT_DEATHMESSAGE", // 19
};

static void
show_interrupt(int indent, struct Interrupt *interrupt)
{
    printf("%*sis_Node.ln_Type=%x %s\n",
           indent, "", interrupt->is_Node.ln_Type,
           interrupt->is_Node.ln_Type < ARRAY_SIZE(interrupt_type_name) ?
           interrupt_type_name[interrupt->is_Node.ln_Type] : "");
    printf("%*sis_Node.ln_Pri=%d\n", indent, "", interrupt->is_Node.ln_Pri);
    printf("%*sis_Node.ln_Name=%p '%s'\n", indent, "",
           interrupt->is_Node.ln_Name, interrupt->is_Node.ln_Name);
    printf("%*sis_Code %p(%p)\n",
           indent, "", interrupt->is_Code, interrupt->is_Data);
}

static void
show_sc_tinfo(int indent_count, struct siop_tinfo *st)
{
    printf("scmds=%d dconns=%d touts=%d perrs=%d\n",
           st->cmds, st->dconns, st->touts, st->perrs);
    printf("%*slubusy=%u flags=%u period=%u offset=%u\n", indent_count, "",
           st->lubusy, st->flags, st->period, st->offset);
}

static void
show_periph(struct scsipi_periph *periph)
{
    int count = 0;
    printf("Periph=%p\n", periph);
    printf("  drv_state=%p\n", periph->drv_state);
    printf("  periph_channel=%p\n", periph->periph_channel);
    printf("  periph_changeintlist=");
    struct IOStdReq *io;
    for (io = (struct IOStdReq *)periph->periph_changeintlist.mlh_Head;
         io->io_Message.mn_Node.ln_Succ != NULL;
         io = (struct IOStdReq *)io->io_Message.mn_Node.ln_Succ) {
        if (count++ == 0)
            printf("\n");
        printf("    ioRequest=%p\n", io);
        printf("      io_Message=%p\n", &io->io_Message);
        printf("      io_Device=%p\n", io->io_Device);
        printf("      io_Unit=%p\n", io->io_Unit);
        printf("      io_Command=%04x %s\n",
               io->io_Command, iocmd_name(io->io_Command));
        decode_io_command(8, (struct IOStdReq *) io);
        printf("      io_Flags=%02x\n", io->io_Flags);
        printf("      io_Error=%02x\n", io->io_Error);
        printf("      io_Data=%p\n", io->io_Data);
        if (io->io_Data != NULL) {
            show_interrupt(8, io->io_Data);
        }
    }
    if (count == 0)
        printf("NONE\n");
    printf("  periph_changeint=%p\n", periph->periph_changeint);
    if (periph->periph_changeint != NULL)
        show_interrupt(4, periph->periph_changeint);
    printf("  periph_openings=%d\n", periph->periph_openings);
    printf("  periph_sent=%d\n", periph->periph_sent);
    printf("  periph_mode=%x\n", periph->periph_mode);
    printf("  periph_period=%d\n", periph->periph_period);
    printf("  periph_offset=%d\n", periph->periph_offset);

    printf("  periph_type=%u  %s\n", periph->periph_type,
           periph->periph_type < ARRAY_SIZE(scsi_periph_type_name) ?
           scsi_periph_type_name[periph->periph_type] : "");
    printf("  periph_cap=0x%x ", periph->periph_cap);
    print_bits(bits_periph_cap, ARRAY_SIZE(bits_periph_cap),
               periph->periph_cap);
    printf("\n");
    printf("  periph_quirks=%x\n", periph->periph_quirks);
    printf("  periph_flags=%x ", periph->periph_flags);
    print_bits(bits_periph_flags, ARRAY_SIZE(bits_periph_flags),
               periph->periph_flags);
    printf("\n");
    printf("  periph_dbflags=%x\n", periph->periph_dbflags);
    printf("  periph_target=%d\n", periph->periph_target);
    printf("  periph_lun=%d\n", periph->periph_lun);
    printf("  periph_blkshift=%d (%u bytes)\n", periph->periph_blkshift,
           1U << periph->periph_blkshift);
    printf("  periph_changenum=%d\n", periph->periph_changenum);
    printf("  periph_tur_active=%d\n", periph->periph_tur_active);
    printf("  periph_version=%d\n", periph->periph_version);
//  printf("  periph_freetags[]=\n", periph->periph_freetags[i]);
//  printf("  periph_xferq=%p%s\n", xq, (xs == NULL) ? "  EMPTY" : "");

#if 0
    struct scsipi_channel *chan = periph->periph_channel;
    struct scsipi_xfer_queue *xq;

    /*
     * periph_xferq is not being tracked in AmigaOS driver,
     * so it is simulated here using the channel xferq.
     */
    Forbid();
    xq = &chan->chan_queue;
    xs = TAILQ_FIRST(xq);
    while ((xs != NULL) && (xs->xs_periph != periph)) {
        /* Channel queue might have other XS than just this periph */
        xs = TAILQ_NEXT(xs, channel_q);
    }
    printf("  periph_xferq=%p%s\n", xq, (xs == NULL) ? "  EMPTY" : "");
    while (xs != NULL) {
        printf("    xs=%p\n", xs);
        print_xs(xs, 6);
        xs = TAILQ_NEXT(xs, channel_q);
        while ((xs != NULL) && (xs->xs_periph != periph)) {
            /* Channel queue might have other XS than just this periph */
            xs = TAILQ_NEXT(xs, channel_q);
        }
    }
    Permit();
#endif

    if (periph->periph_callout.func == NULL) {
        printf("  periph_callout NONE\n");
    } else {
        printf("  periph_callout ticks=%d\n", periph->periph_callout.ticks);
        printf("                 func=%p(%p)\n",
               periph->periph_callout.func, periph->periph_callout.arg);
    }
    printf("  periph_xscheck=%p\n", periph->periph_xscheck);
    Forbid();
    if (periph->periph_xscheck != NULL)
        print_xs(periph->periph_xscheck, 6);
    Permit();
}

int
main(int argc, char *argv[])
{
    int unitno = -1;
    int arg;
    int pos = 0;
    int rc = 0;
    int open_and_wait = 0;
    struct IOExtTD     *tio;
    struct MsgPort     *mp;
    struct IOStdReq    *ior;
    struct scsipi_xfer *xs;
    struct scsipi_periph *periph;
    char   *devname = DEVICE_NAME;

    for (arg = 1; arg < argc; arg++) {
        char *ptr = argv[arg];
        if (*ptr == '-') {
            while (*(++ptr) != '\0') {
                switch (*ptr) {
                    case 'p': {
                        if (++arg > argc) {
                            printf("-%c requires an argument\n", *ptr);
                            exit(1);
                        }
                        if ((sscanf(argv[arg], "%x%n",
                                    (uint *) &periph, &pos) != 1) ||
                            (argv[arg][pos] != '\0')) {
                            printf("Invalid periph address '%s'\n", argv[arg]);
                        }
                        show_periph(periph);
                        exit(0);
                    }
                    case 'x':
                        if (++arg > argc) {
                            printf("-%c requires an argument\n", *ptr);
                            exit(1);
                        }
                        if ((sscanf(argv[arg], "%x%n",
                                    (uint *) &xs, &pos) != 1) ||
                            (argv[arg][pos] != '\0')) {
                            printf("Invalid xs address '%s'\n", argv[arg]);
                        }
                        print_xs(xs, 1);
                        exit(0);
                    case 'w':
                        open_and_wait++;
                        break;
                    default:
                        printf("Invalid argument -%s\n", ptr);
                        usage();
                        exit(1);
                }
            }
        } else {
            if ((sscanf(ptr, "%d%n", &unitno, &pos) != 1) ||
                (ptr[pos] != '\0')) {
                printf("Invalid unit '%s'\n", ptr);
                usage();
                exit(1);
            }
        }
    }

    if (unitno == -1) {
        usage();
        exit(1);
    }

    __check_abort_enabled = 0;  // Disable gcc clib2 ^C break handling

    mp = CreatePort(NULL, 0);
    if (mp == NULL) {
        printf("Failed to create message port\n");
        exit(1);
    }

    tio = (struct IOExtTD *) CreateExtIO(mp, sizeof (struct IOExtTD));
    if (tio == NULL) {
        printf("Failed to create tio struct\n");
        rc = 1;
        goto extio_fail;
    }

    if (OpenDevice(devname, unitno, (struct IORequest *) tio, TDF_DEBUG_OPEN)) {
        printf("Unit %d is not currently open; attemping a normal open.\n",
               unitno);
        if (OpenDevice(devname, unitno, (struct IORequest *) tio, 0)) {
            printf("Open %s failed\n", devname);
            rc = 1;
            goto open_fail;
        }
    }
    global_opened = 1;
    global_mp     = mp;
    global_tio    = tio;

    if (open_and_wait) {
        int i;
        printf("Device open; press enter to proceed.\n");
        scanf("%d", &i);
    }

    ior = &tio->iotd_Req;
    struct MsgPort *rp = ior->io_Message.mn_ReplyPort;
    struct Library *dp = &ior->io_Device->dd_Library;
    printf("IORequest\n");
    printf("  io_Message=%p\n", &ior->io_Message);
    printf("    mn_Node=%p\n", &ior->io_Message.mn_Node);
    printf("    mn_ReplyPort=%p\n", ior->io_Message.mn_ReplyPort);
    printf("      mp_Node=%p\n", &rp->mp_Node);
    printf("      mp_Flags=%02x\n", rp->mp_Flags);
    printf("      mp_SigBit=%02x\n", rp->mp_SigBit);
    printf("      mp_SigTask=%p\n", rp->mp_SigTask);
    printf("      mp_MsgList=%p\n", &rp->mp_MsgList);
    printf("    mn_Length=%04x\n", ior->io_Message.mn_Length);
    printf("  io_Device=%p\n", ior->io_Device);
    printf("    lib_Node=%p\n", &dp->lib_Node);
    printf("    lib_Flags=%02x\n", dp->lib_Flags);
    printf("    lib_pad=%02x\n", dp->lib_pad);
    printf("    lib_NegSize=%04x\n", dp->lib_NegSize);
    printf("    lib_PosSize=%04x\n", dp->lib_PosSize);
    printf("    lib_Version=%04x\n", dp->lib_Version);
    printf("    lib_Revision=%04x\n", dp->lib_Revision);
    printf("    lib_IdString=%p \"%.40s\"\n",
           dp->lib_IdString, (char *)dp->lib_IdString);
    printf("    lib_Sum=%08x\n", (unsigned int) dp->lib_Sum);
    printf("    lib_OpenCnt=%04x\n", dp->lib_OpenCnt);
    if (is_user_abort())
        close_exit();

    struct Unit *u = ior->io_Unit;
    printf("  io_Unit=%p\n", u);
    struct MsgPort *ump = &u->unit_MsgPort;
    printf("    unit_MsgPort=%p\n", &u->unit_MsgPort);
    printf("      mp_Node=%p\n", &ump->mp_Node);
    printf("      mp_Flags=%02x\n", ump->mp_Flags);
    printf("      mp_SigBit=%02x\n", ump->mp_SigBit);
    printf("      mp_SigTask=%p\n", ump->mp_SigTask);
    printf("      mp_MsgList=%p\n", &ump->mp_MsgList);
    printf("    unit_flags=%02x\n", u->unit_flags);
    printf("    unit_pad=%02x\n", u->unit_pad);
    printf("    unit_OpenCnt=%04x\n", u->unit_OpenCnt);

    printf("  io_Command=%04x %s\n",
           ior->io_Command, iocmd_name(ior->io_Command));
    printf("  io_Flags=%02x\n", ior->io_Flags);
    printf("  io_Error=%02x", ior->io_Error);
    if (ior->io_Error != 0)
        decode_io_error(ior->io_Error);
    printf("\n");
    printf("  iotd_Count=%08x\n", ior->io_Error);
    printf("  iotd_SecLabel=%08x\n", ior->io_Error);

    if (is_user_abort())
        close_exit();

    periph = (void *) u;
    struct scsipi_channel *chan = periph->periph_channel;
    show_periph(periph);

    if (is_user_abort())
        close_exit();

    printf("Chan %p\n", chan);
    Forbid();
    xs = chan->chan_xs_free;
    printf("  chan_xs_free=");
    if (xs == NULL)
        printf("NONE");

    while (xs != NULL) {
        printf("%p", xs);
        xs = *(struct scsipi_xfer **) xs;  /* ->next link on free list */
        if (xs != NULL)
            printf(" ");
    }
    printf("\n");
    xs = chan->chan_xs_free;
    if (xs != NULL) {
        printf("    xs=%p\n", xs);
        print_xs(xs, 6);
    }
    Permit();

    if (is_user_abort())
        close_exit();

    printf("  chan_active=%d\n", chan->chan_active);
    struct scsipi_adapter *adapt = chan->chan_adapter;
    printf("  chan_adapter=%p\n", adapt);
    printf("    adapt_dev=%p\n", adapt->adapt_dev);
    printf("    adapt_nchannels=%d\n", adapt->adapt_nchannels);
    printf("    adapt_refcnt=%d\n", adapt->adapt_refcnt);
    printf("    adapt_openings=%d\n", adapt->adapt_openings);
//  printf("    adapt_max_periph=%d\n", adapt->adapt_max_periph);
    printf("    adapt_flags=%d\n", adapt->adapt_flags);
    printf("    adapt_runnings=%d\n", adapt->adapt_running);
    printf("    adapt_asave=%p\n", adapt->adapt_asave);
    printf("  chan_periphtab[]=%p\n", chan->chan_periphtab);
    for (int i = 0; i < SCSIPI_CHAN_PERIPH_BUCKETS; i++) {
        LIST_FOREACH(periph, &chan->chan_periphtab[i], periph_hash) {
            printf("    periph=%p %d.%d\n", periph,
                   periph->periph_target, periph->periph_lun);
        }
    }

    printf("  chan_flags=%02x", chan->chan_flags);
    print_bits(bits_chan_flags, ARRAY_SIZE(bits_chan_flags), chan->chan_flags);
    printf("\n");
    printf("  chan_openings=%d\n", chan->chan_openings);
    printf("  chan_nluns=%d\n", chan->chan_nluns);
    printf("  chan_id=%d (SCSI host ID)\n", chan->chan_id);
    printf("  chan_tflags=%d", chan->chan_tflags);
    print_bits(bits_chan_tflags, ARRAY_SIZE(bits_chan_tflags),
               chan->chan_tflags);
    printf("\n");

    if (is_user_abort())
        close_exit();

    Forbid();
    struct scsipi_xfer_queue *xq = &chan->chan_queue;
    xs = TAILQ_FIRST(xq);
    printf("  chan_queue=%p%s\n", xq, (xs == NULL) ? "  EMPTY" : "");
    while (xs != NULL) {
        printf("    xs=%p", xs);
        if (xs->xs_periph != NULL) {
            struct scsipi_periph *tperiph = xs->xs_periph;
            printf("  periph %d.%d",
                   tperiph->periph_target, tperiph->periph_lun);
            if (tperiph == periph)
                printf(" (THIS)");
        }
        printf("\n");
        print_xs(xs, 6);
        xs = TAILQ_NEXT(xs, channel_q);
    }
    Permit();

    if (is_user_abort())
        close_exit();

    Forbid();
    xq = &chan->chan_complete;
    xs = TAILQ_FIRST(xq);
    printf("  chan_complete=%p%s\n", xq, (xs == NULL) ? "  EMPTY" : "");
    while (xs != NULL) {
        printf("    xs=%p\n", xs);
        print_xs(xs, 6);
        xs = TAILQ_NEXT(xs, channel_q);
    }
    Permit();
    a4091_save_t *asave = adapt->adapt_asave;
    if (asave != NULL) {
        printf("Driver globals %p\n", asave);
        printf("  as_SysBase=%p\n", asave->as_SysBase);
        printf("  as_timer_running=%x\n", asave->as_timer_running);
        printf("  as_irq_signal=%x\n", asave->as_irq_signal);
        printf("  as_irq_count=%x\n", asave->as_irq_count);
        printf("  as_int_mask=%08x\n", asave->as_int_mask);
        printf("  as_timer_mask=%08x\n", asave->as_timer_mask);
        printf("  as_svc_task=%p\n", asave->as_svc_task);
        printf("  as_isr=%p\n", asave->as_isr);
        show_interrupt(4, asave->as_isr);
        printf("  as_exiting=%x\n", asave->as_exiting);
        printf("  as_device_self=%p\n", &asave->as_device_self);
        printf("  as_device_private=%p\n", asave->as_device_private);
        struct siop_softc *sc = asave->as_device_private;
//      printf("    sc_siop_si=%p\n", sc->sc_siop_si);
        printf("    sc_istat=%u sc_dstate=%u sc_sstat0=%u sc_sstat1=%u\n",
               sc->sc_istat, sc->sc_dstat, sc->sc_sstat0, sc->sc_sstat1);
        printf("    sc_intcode=%04lx\n", sc->sc_intcode);
        printf("    sc_adapter=%p\n", &sc->sc_adapter);
        printf("    sc_channel=%p\n", &sc->sc_channel);
        printf("    sc_scriptspa=%lx\n", sc->sc_scriptspa);
        printf("    sc_siopp=%p\n", sc->sc_siopp);
        printf("    sc_active=%lu\n", sc->sc_active);

        Forbid();
        printf("    free_list=");
        show_sc_list(6, sc->free_list.tqh_first);
        printf("    ready_list=");  // Queue of xs to be issued
        show_sc_list(6, sc->ready_list.tqh_first);
        printf("    nexus_list=");  // List of xs already issued on channel
        show_sc_list(6, sc->nexus_list.tqh_first);
        Permit();

        if (is_user_abort())
            close_exit();

        Forbid();
        printf("    sc_nexus=%p", sc->sc_nexus);  // Current active xs
        decode_acb(sc->sc_nexus);
        if ((sc->sc_active != 0) &&
            (sc->sc_nexus != NULL) && (sc->sc_nexus->xs != NULL)) {
            /* Do full decode of ACB */
            print_xs(sc->sc_nexus->xs, 6);
        }
        Permit();
        printf("    sc_acb[0]=%p\n", sc->sc_acb);
        for (pos = 0; pos < ARRAY_SIZE(sc->sc_tinfo); pos++) {
            printf("    sc_tinfo[%d] ", pos);
            show_sc_tinfo(16, &sc->sc_tinfo[pos]);
        }
        printf("    sc_clock_freq=%d MHz\n", sc->sc_clock_freq);
        printf("    sc_dcntl=%02x sc_ctest7=%02x sc_tcp[]=%04x %04x "
               "%04x %04x\n",
               sc->sc_dcntl, sc->sc_ctest7, sc->sc_tcp[0],
               sc->sc_tcp[1], sc->sc_tcp[2], sc->sc_tcp[3]);
        printf("    sc_flags=%02x sc_dien=%02x sc_minsync=%02x "
               "sc_sien=%02x\n",
               sc->sc_flags, sc->sc_dien, sc->sc_minsync, sc->sc_sien);
        printf("    sc_nosync=%x sc_nodisconnect=%x\n",
               sc->sc_nosync, sc->sc_nodisconnect);
        for (pos = 0; pos < ARRAY_SIZE(sc->sc_sync); pos++) {
            printf("    sc_sync[%d] state=%u sxfer=%u sbcl=%u\n",
                   pos, sc->sc_sync[pos].state, sc->sc_sync[pos].sxfer,
                   sc->sc_sync[pos].sbcl);
        }

        if (is_user_abort())
            close_exit();

        printf("  as_timerport[0]=%p as_timerport[1]=%p\n",
                asave->as_timerport[0], asave->as_timerport[1]);
        printf("  as_timerio[0]=%p   as_timerio[1]=%p\n",
               asave->as_timerio[0], asave->as_timerio[1]);
        callout_t *callout_head = *(asave->as_callout_head);
        callout_t *cur;
        printf("  as_callout_head=%p\n", callout_head);
        for (cur = callout_head; cur != NULL; cur = cur->co_next) {
            printf("    %p ticks=%d func=%p(%p)\n",
                   cur, cur->ticks, cur->func, cur->arg);
        }
    }

    CloseDevice((struct IORequest *) tio);

open_fail:
    DeleteExtIO((struct IORequest *) tio);

extio_fail:
    DeletePort(mp);
    global_opened = 0;
    exit(rc);
}
