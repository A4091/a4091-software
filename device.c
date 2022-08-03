#include "port.h"
#include <devices/trackdisk.h>
#include <dos/dostags.h>
#include <exec/errors.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/semaphores.h>
#include <exec/io.h>
#include <libraries/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <utility/tagitem.h>
#include <clib/alib_protos.h>
#include <devices/scsidisk.h>

#include "device.h"
#include "cmdhandler.h"
#include "version.h"

#ifdef DEBUG
#include <clib/debug_protos.h>
#endif

#define STR(s) #s      // Turn s into a string literal without macro expansion
#define XSTR(s) STR(s) // Turn s into a string literal after macro expansion

#define DRIVER      "a4091"
#define DEVICE_VERSION  1
#define DEVICE_REVISION 0
#define DEVICE_PRIORITY 0  // Fine to leave priority as zero

#define DEVICE_NAME DRIVER".device"
#define DEVICE_ID_STRING DRIVER " " XSTR(DEVICE_VERSION) "." \
        XSTR(DEVICE_REVISION) " (" BUILD_DATE ")"
        /* format: "name version.revision (yyyy-mm-dd)" */

struct ExecBase *SysBase;
struct MsgPort *myPort;

BPTR saved_seg_list;

/*
 * -----------------------------------------------------------
 * A library or device with a romtag should start with moveq #-1,d0
 * (to safely return an error if a user tries to execute the file),
 * followed by a Resident structure.
 * ------------------------------------------------------------
 */
int __attribute__((no_reorder)) _start()
{
    return (-1);
}

/*
 * -----------------------------------------------------------
 * A romtag structure. After the driver is brought in from disk, the
 * disk image will be scanned for this structure to discover magic
 * constants about it (such as where to start running from...).
 *
 * endcode is a marker that shows the end of your code. Make sure it
 * does not span hunks, and is not before the rom tag! It is ok to
 * put it right after the rom tag -- that way you it's always safe.
 * Make sure the program has only a single code hunk if you put it
 * at the end of your code.
 * ------------------------------------------------------------
 */
asm("romtag:                                \n"
    "       dc.w    "XSTR(RTC_MATCHWORD)"   \n"
    "       dc.l    romtag                  \n"
    "       dc.l    endcode                 \n"
    "       dc.b    "XSTR(RTF_AUTOINIT)"    \n"
    "       dc.b    "XSTR(DEVICE_VERSION)"  \n"
    "       dc.b    "XSTR(NT_DEVICE)"       \n"
    "       dc.b    "XSTR(DEVICE_PRIORITY)" \n"
    "       dc.l    _device_name            \n"
    "       dc.l    _device_id_string       \n"
    "       dc.l    _auto_init_tables       \n"
    "endcode:                               \n");

const char device_name[]      = DEVICE_NAME;
const char device_id_string[] = DEVICE_ID_STRING;
struct SignalSemaphore entry_sem;

/*
 * ------- init_device ---------------------------------------
 * FOR RTF_AUTOINIT:
 *   This routine gets called after the device has been allocated.
 *   The device pointer is in d0. The AmigaDOS segment list is in a0.
 *   If it returns the device pointer, then the device will be linked
 *   into the device list.  If it returns NULL, then the device
 *   will be unloaded.
 *
 * IMPORTANT:
 *   If you don't use the "RTF_AUTOINIT" feature, there is an additional
 *   caveat. If you allocate memory in your Open function, remember that
 *   allocating memory can cause an Expunge... including an expunge of your
 *   device. This must not be fatal. The easy solution is don't add your
 *   device to the list until after it is ready for action.
 *
 * CAUTION:
 *   This function runs in a forbidden state !!!
 *   This call is single-threaded by Exec
 * ------------------------------------------------------------
 */
static struct Library __attribute__((used)) *
init_device(BPTR seg_list asm("a0"), struct Library *dev asm("d0"))
{
    /* !!! required !!! save a pointer to exec */
    SysBase = *(struct ExecBase **)4UL;

    /* save pointer to our loaded code (the SegList) */
    saved_seg_list = seg_list;

    dev->lib_Node.ln_Type = NT_DEVICE;
    dev->lib_Node.ln_Name = (char *) device_name;
    dev->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    dev->lib_Version = DEVICE_VERSION;
    dev->lib_Revision = DEVICE_REVISION;
    dev->lib_IdString = (APTR)device_id_string;

    /* Start thread to manage board and process commands */
    InitSemaphore(&entry_sem);
    ObtainSemaphore(&entry_sem);
    printf("A4091 driver %s %s\n", device_name, device_id_string);

    int scsi_unit = 0;
    if (start_cmd_handler(&scsi_unit)) {
        printf("Start handler failed\n");
        return (NULL);
    }
    ReleaseSemaphore(&entry_sem);

    return (dev);
}

/*
 * device dependent expunge function
 * ------------------------------------------------------------
 * !!! CAUTION: This function runs in a forbidden state !!!
 * This call is guaranteed to be single-threaded; only one task
 * will execute your Expunge at a time.
 */
static BPTR __attribute__((used))
drv_expunge(struct Library *dev asm("a6"))
{
    ObtainSemaphore(&entry_sem);
    if (dev->lib_OpenCnt != 0) {
        printf("expunge() device still open\n");
        dev->lib_Flags |= LIBF_DELEXP;
        ReleaseSemaphore(&entry_sem);
        return (0);
    }

    printf("expunge() %s\n", version_str);

    /* Ask the command handler to shut down */
    stop_cmd_handler();

    Forbid();
    BPTR seg_list = saved_seg_list;
    Remove(&dev->lib_Node);
    FreeMem((char *)dev - dev->lib_NegSize,
            dev->lib_NegSize + dev->lib_PosSize);
    ReleaseSemaphore(&entry_sem);
    return (seg_list);
}

/*
 * drv_open
 * --------
 * Opens the device for access. The first call of this function
 * will perform an attach (initialize) of the SCSI adapter.
 *
 * ------------------------------------------------------------
 *
 * CAUTION: This function runs in Forbid() state. It is guaranteed
 *          to be single-threaded; only one task will execute Open()
 *          at a time.
 */
void __attribute__((used))
drv_open(struct Library *dev asm("a6"), struct IORequest *ioreq asm("a1"),
         uint scsi_unit asm("d0"), ULONG flags asm("d1"))
{
    ioreq->io_Message.mn_Node.ln_Type = NT_REPLYMSG;

    if (SysBase->LibNode.lib_Version < 36) {
        ioreq->io_Error = IOERR_OPENFAIL;
        return; /* can only run under 2.0 or greater */
    }

    ObtainSemaphore(&entry_sem);
    dev->lib_OpenCnt++;

    if (open_unit(scsi_unit, (void **) &ioreq->io_Unit)) {
        printf("Open fail %d.%d\n", scsi_unit % 10, scsi_unit / 10);
        dev->lib_OpenCnt--;
        ioreq->io_Error = HFERR_SelTimeout;
// HFERR_SelfUnit - attempted to open our own SCSI ID
        ReleaseSemaphore(&entry_sem);
        return;
    }
#if 0
    printf("Open Unit=%p\n", ioreq->io_Unit);
#endif

    ioreq->io_Error = 0; // Success

    ReleaseSemaphore(&entry_sem);
}

/*
 * drv_close
 * ---------
 * Closes the device. The last call of this function will perform an detach
 * (reset) of the SCSI adapter.
 *
 * ------------------------------------------------------------
 *
 * CAUTION: This function runs in Forbid() state. It is guaranteed
 *          to be single-threaded; only one task will execute Close()
 *          at a time.
 */
static BPTR __attribute__((used))
drv_close(struct Library *dev asm("a6"), struct IORequest *ioreq asm("a1"))
{
    ObtainSemaphore(&entry_sem);
#if 0
    printf("close %u\n", dev->lib_OpenCnt);
#endif

    close_unit((void *) ioreq->io_Unit);

    dev->lib_OpenCnt--;
    ReleaseSemaphore(&entry_sem);
    if (dev->lib_Flags & LIBF_DELEXP)
        return (drv_expunge(dev));
    return (0);
}

/* device dependent beginio function */
static void __attribute__((used))
drv_begin_io(struct Library *dev asm("a6"), struct IORequest *ior asm("a1"))
{
    ior->io_Flags &= ~IOF_QUICK;
    PutMsg(myPort, &ior->io_Message);
}

/* device dependent abortio function */
static ULONG __attribute__((used))
drv_abort_io(struct Library *dev asm("a6"), struct IORequest *ior asm("a1"))
{
    printf("abort_io(%d)\n", ior->io_Command);

    return (IOERR_NOCMD);
}

static ULONG device_vectors[] =
{
    (ULONG) drv_open,
    (ULONG) drv_close,
    (ULONG) drv_expunge,
    0,   // extFunc not used here
    (ULONG) drv_begin_io,
    (ULONG) drv_abort_io,
    -1   // function table end marker
};

/*
 * ------------------------------------------------------------
 * The romtag specified that we were "RTF_AUTOINIT".  This means
 * that the RT_INIT structure member points to one of these
 * tables below. If the AUTOINIT bit was not set then RT_INIT
 * would point to a routine to run.
 *
 * MyDev_Sizeof    data space size
 * device_vectors  pointer to function initializers
 * dataTable       pointer to data initializers
 * init_device     routine to run
 * ------------------------------------------------------------
 */
const ULONG auto_init_tables[4] =
{
        sizeof (struct Library),
        (ULONG) device_vectors,
        0,
        (ULONG) init_device
};
