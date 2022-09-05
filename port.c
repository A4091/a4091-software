#ifdef DEBUG_PORT
#define USE_SERIAL_OUTPUT
#endif

#include "port.h"

#include <stdlib.h>
#include <string.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <clib/debug_protos.h>
#include <clib/exec_protos.h>
#include <exec/execbase.h>
#include "device.h"
#include "printf.h"

#include "scsipiconf.h"
#include "siopreg.h"
#include "siopvar.h"
#include "attach.h"

#ifdef DEBUG_CALLOUT
#define PRINTF_CALLOUT(args...) printf(args)
#else
#define PRINTF_CALLOUT(args...)
#endif

#undef panic
void
panic(const char *s)
{
    printf("PANIC: %s", s);
}

#if 0
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
#endif

extern a4091_save_t *asave;

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

    // We opened timer.device with UNIT_VBLANK, so
    // we use ticks here.
    struct timerequest *TimerIO = asave->as_timerio[1];
    TimerIO->tr_node.io_Command = TR_ADDREQUEST;
    TimerIO->tr_time.tv_secs  = 0;
    TimerIO->tr_time.tv_micro = ticks;
    DoIO((struct IORequest *)TimerIO);
}

static int bsd_ilevel = 0;
/* Block (nesting) interrupts */
int
bsd_splbio(void)
{
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
}

#ifdef USE_SERIAL_OUTPUT
const char *
device_xname(void *ptr)
{
    device_t dev = ptr;
    if (dev == NULL)
        return ("SCSI");
    else
        return (dev->dv_xname);
}

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

char *itoa(int value, char *string, int radix)
{
    int n,r,a,b = 0;
    n = (value<0)?-value:value;
    b = 0;
    while (n)
    {
        r = n % radix;
        if (r >= 10)
            string[b++] = 'a' + (r - 10);
        else
            string[b++] = '0' + r;
        n /= radix;
    }
    if (b == 0)
        string[b++] = '0';
    if (value < 0 && radix == 10)
        string[b++] = '-';
    string[b] = '\0';
    a=0; b--;
    while (a < b) {
        char temp = string[a];
        string[a] = string[b];
        string[b] = temp;
        a++;b--;
    }
    return string;
}

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

callout_t *callout_head = NULL;

static void
callout_add(callout_t *c)
{
    c->co_next = callout_head;
    c->co_prev = NULL;
    if (callout_head != NULL) {
        callout_head->co_next->co_prev = c;
    }
    callout_head = c;
}

static void
callout_remove(callout_t *c)
{
    if (c == callout_head) {
        if (c->co_prev != NULL) {
            printf("CALLOUT head %p has non-NULL prev %p\n",
                   callout_head, c->co_prev);
            c->co_prev = NULL;
        }
        callout_head = c->co_next;
    } else if (c->co_prev != NULL) {
        c->co_prev->co_next = c->co_next;
    } else if (c->co_next != NULL) {
        printf("CALLOUT list corrupt head=%p c=%p\n", callout_head, c);
    }

    if (c->co_next != NULL)
        c->co_next->co_prev = c->co_prev;
}

void
callout_init(callout_t *c, u_int flags)
{
    c->func = NULL;
}

#ifdef DEBUG
void
callout_list(void)
{
    callout_t *cur;

    for (cur = callout_head; cur != NULL; cur = cur->co_next) {
        printf("%c %d %p(%p)\n", (cur == callout_head) ? '>' : ' ',
               cur->ticks, cur->func, cur->arg);
    }
}
#endif

int
callout_pending(callout_t *c)
{
    PRINTF_CALLOUT("callout %spending\n", (c->func == NULL) ? "not " : "");
    return (c->func != NULL);
}

int
callout_stop(callout_t *c)
{
    int pending = (c->func != NULL);
    PRINTF_CALLOUT("callout stop %p\n", c->func);
    c->func = NULL;
    callout_remove(c);
    return (pending);
}

void
callout_reset(callout_t *c, int ticks, void (*func)(void *), void *arg)
{
    c->ticks = ticks;
    c->func = func;
    c->arg = arg;

    callout_remove(c);
    callout_add(c);
    PRINTF_CALLOUT("callout_reset %p(%x) at %d\n",
                   c->func, (uint32_t) c->arg, ticks);
}

void
callout_call(callout_t *c)
{
    if (c->func == NULL) {
        PRINTF_CALLOUT("callout to NULL function\n");
        return;
    }

    PRINTF_CALLOUT("callout_call %p(%x)\n", c->func, (uint32_t) c->arg);
    c->func(c->arg);
}

void
callout_run_timeouts(void)
{
    callout_t *cur;

    for (cur = callout_head; cur != NULL; cur = cur->co_next) {
        if (cur->ticks == 1) {
            cur->ticks = 0;
            callout_call(cur);
        } else if (cur->ticks != 0) {
            cur->ticks -= TICKS_PER_SECOND;
            if (cur->ticks < 1)
                cur->ticks = 1;
        }
    }
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
