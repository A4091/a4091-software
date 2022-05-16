#include "port.h"

#include <stdlib.h>
#include <string.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <clib/debug_protos.h>
#include <exec/execbase.h>
#include "device.h"
#include "printf.h"

/* Not sure this even works for a driver */
ulong __stack_size = 32768;

void
panic(const char *s)
{
    printf("PANIC: %s", s);
}


/*
 * I don't know what is dragging this junk in from libc, but stubbing
 * them here reduces the object size by ~12k
 */
const struct __sFILE_fake __sf_fake_stdin =
    {_NULL, 0, 0, 0, 0, {_NULL, 0}, 0, _NULL};
const struct __sFILE_fake __sf_fake_stdout =
    {_NULL, 0, 0, 0, 0, {_NULL, 0}, 0, _NULL};
const struct __sFILE_fake __sf_fake_stderr =
    {_NULL, 0, 0, 0, 0, {_NULL, 0}, 0, _NULL};




#if 0
void
usleep(int usecs)
{
    /*
     * 1. Allocate IORequest
     * 2. Open Message port
     * 3. Open timer.device with VBLANK and MICROHZ
     * 4. DoIO request
     * 5. Close device
     */
}
#endif

void
delay(int usecs)
{
    int msec = usecs / 1000;
    int ticks = TICKS_PER_SECOND * msec / 1000;

    if (ticks == 0) {
        usecs <<= 3;
        for (volatile int i = usecs; i > 0; i--)
            ;
        return;
    }
#if 0
    printf("delay(%u)\n", usecs);
#endif

    Delay(ticks);
}

static int bsd_ilevel = 0;
/* Block (nesting) interrupts */
int
bsd_splbio(void)
{
#undef DEBUG_IRQ
#ifdef DEBUG_IRQ
    printf("splbio() = %d\n", bsd_ilevel);
#endif
    Disable();
    return (bsd_ilevel++);
}

/* Enable (nesting) interrupts */
void
bsd_splx(int ilevel)
{
    bsd_ilevel = ilevel;
    if (bsd_ilevel == 0)
        Enable();
#ifdef DEBUG_IRQ
    printf("splx(%d)\n", ilevel);
#endif
}

const char *
device_xname(void *ptr)
{
    device_t dev = ptr;
    if (dev == NULL)
        return ("SCSI");
    else
        return (dev->dv_xname);
}

#ifndef NO_SERIAL_OUTPUT
unsigned int
read_system_ticks(void)
{
    struct DateStamp ds;
    DateStamp(&ds);  /* Measured latency is ~250us on A3000 A3640 */
    return ((unsigned int) (ds.ds_Minute) * 60 * TICKS_PER_SECOND + ds.ds_Tick);
}

unsigned int
ticks_since_last(void)
{
    unsigned int etime = read_system_ticks();
    unsigned int stime;
    static unsigned int last = 0;
    stime = last;
    last = etime;
    if (etime < stime)
        etime += 24 * 60 * 60 * TICKS_PER_SECOND;  /* Next day */
    return (etime - stime);
}
#endif

#if 0
void exit(int __status)
{
    KPrintF((CONST_STRPTR) "exit(%ld)", errno);
    while (1)
        ;
}
#endif

#if 0
/* Return the index of the lowest set bit. (Counted from one) */
int
ffs(int i)
{
    int result = 0;

    if (i != 0) {
        int x;

        x = (i & (-i)) - 1;
        x -= ((x >> 1) & 0x55555555);
        x = ((x >> 2) & 0x33333333) + (x & 0x33333333);
        x = ((x >> 4) + x) & 0x0f0f0f0f;
        x += (x >> 8);
        x += (x >> 16);

        result = 1 + (x & 0x3f);  /* The first bit has index 1. */
    }

    return (result);
}
#endif
