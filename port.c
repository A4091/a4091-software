#ifndef DEBUG_PORT
#define NO_SERIAL_OUTPUT
#endif

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

void
callout_init(callout_t *c, u_int flags)
{
    c->func = NULL;
}

int
callout_pending(callout_t *c)
{
#if 0
    if (c->func != NULL)
        printf("callout pending\n");
    else
        printf("callout not pending\n");
#endif
    return (c->func != NULL);
}

int
callout_stop(callout_t *c)
{
    int pending = (c->func != NULL);
#if 0
    printf("callout_stop %p\n", c->func);
#endif
    c->func = NULL;
    return (pending);
}

void
callout_reset(callout_t *c, int ticks, void (*func)(void *), void *arg)
{
    c->ticks = ticks;
    c->func = func;
    c->arg = arg;
#if 0
    printf("callout_reset %p(%x) at %d\n", c->func, (uint32_t) c->arg, ticks);
#endif
}

void
callout_call(callout_t *c)
{
    if (c->func == NULL) {
        printf("callout to NULL function\n");
        return;
    }
#if 0
    printf("callout_call %p(%x)\n", c->func, (uint32_t) c->arg);
#endif
    c->func(c->arg);
}


#if 0
void
callout_destroy(callout_t *c);


void
callout_schedule(callout_t *c, int ticks);

void
callout_setfunc(callout_t *c, void (*func)(void *), void *arg);

bool
callout_halt(callout_t *c, void *interlock);


bool
callout_expired(callout_t *c);

bool
callout_active(callout_t *c);

bool
callout_invoking(callout_t *c);

void
callout_ack(callout_t *c);
#endif

