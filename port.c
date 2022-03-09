#include "port.h"

#include <stdlib.h>
#include <string.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <clib/debug_protos.h>
#include "printf.h"

void check_break(void);

unsigned long scsi_nosync = 0;
unsigned long shift_nosync = 0;

void
panic(const char *s)
{
    printf("PANIC: %s", s);
    exit(1);
}

void
delay(int usecs)
{
    int msec = usecs / 1000;
    int ticks = TICKS_PER_SECOND * msec / 1000;
//  static int count = 0;
    if ((ticks < 1) && (usecs > 1000))
        ticks = 1;

#if 0
    if (count < 10) {
        count++;
        printf("delay(%d)\n", usecs);
    }
#endif
    Delay(ticks);
//  if (ticks > 1)
        check_break();
}

static int bsd_ilevel = 0;
int
bsd_splbio(void)
{
#undef DEBUG_IRQ
#ifdef DEBUG_IRQ
    printf("splbio() = %d\n", bsd_ilevel);
    check_break();
#endif
    if (bsd_ilevel == 0)
        Disable();
    return (bsd_ilevel++);
}

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
    return ("SCSI");
}

int
mstohz(int m)
{
    int hz = 50;
    int h = m;

    if (h > 0) {
        h = h * hz / 1000;
        if (h == 0)
            h = 1000 / hz;
    }

    return (h);
}

void
callout_reset(void *cs, int to_ticks, void (*func)(void *), void *arg)
{
    printf("callout_reset()\n");
    check_break();
}

int
callout_stop(void *cs)
{
    /* try to cancel a pending callout */
    printf("callout_stop()\n");
    check_break();
    return (0);
}


uint32_t
kvtop(void *vaddr)
{
#if 0
    static uint32_t count = 0;
    while (count++ < 10)
        printf("kvtop(%p)\n", vaddr);
    check_break();
#endif
    return ((uint32_t) vaddr);
}

// #define DCIAS(x) /* cache flush: cpushl dc,%a0@ */
void
_DCIAS(uint32_t paddr)
{
    /* Cache flush of some sort */
#if 0
    printf("DCIAS(%x)\n", paddr);
#endif
    check_break();
}

void
_DCIU(void)
{
    printf("_DCIU()\n");
}

void
_ICIA(void)
{
    printf("_ICIA()\n");
}


int
dma_cachectl(void *addr, int len)
{
    printf("dma_cachectl(%p, %x)\n", addr, len);
    check_break();
    return (0);
}


ulong __stack_size = 32768;

#if 0
void *
local_memcpy(void *dst, const void *src, size_t len)
{
    CopyMem((void *)src, dst, len);
    return (dst);
}
#endif
#if 0
void *
local_memset(void *dst, int value, size_t len)
{
    SetMem(dst, value, len);
    return (dst);
}
#endif

void
check_break(void)
{
}

#if 1
void exit(int __status)
{
    KPrintF((CONST_STRPTR) "exit(%ld)", errno);
}
#endif

void
Zignal(struct Task *task, ULONG signalSet)
{
}

/* Return the index of the lowest set bit. (Counted from one) */
int
ffs(int i)
{
        int result = 0;

        if(i != 0)
        {
                int x;

                x = (i & (-i)) - 1;
                x -= ((x >> 1) & 0x55555555);
                x = ((x >> 2) & 0x33333333) + (x & 0x33333333);
                x = ((x >> 4) + x) & 0x0f0f0f0f;
                x += (x >> 8);
                x += (x >> 16);

                result = 1 + (x & 0x3f); /* The first bit has index 1. */
        }

        return(result);
}
