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

u_long scsi_nosync = 0;
int    shift_nosync = 0;

void
panic(const char *s)
{
    printf("PANIC: %s", s);
    exit(1);
}

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

#if 0
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
}

int
callout_stop(void *cs)
{
    /* try to cancel a pending callout */
    printf("callout_stop()\n");
    return (0);
}

uint32_t
kvtop(void *vaddr)
{
    return ((uint32_t) vaddr);
}

// #define DCIAS(x) /* cache flush: cpushl dc,%a0@ */
void
_DCIAS(uint32_t paddr)
{
    /* Cache flush of some sort */
}

/* cache flush / purge */
void
_DCIU(void)
{
    printf("_DCIU()\n");
}

/* instruction cache purge */
void
_ICIA(void)
{
    printf("_ICIA()\n");
}
#endif


#if 0
/*
 * dma_cachectl
 * ------------
 * Flush CPU cache at specified address to memory in preparation for a
 * device-initiated DMA read to occur.
 */
int
dma_cachectl(void *addr, int len)
{
//  printf("dma_cachectl(%p, %x)\n", addr, len);
    ULONG flags = DMA_ReadFromRAM;
    int paddr = (int) addr;

    while (len > 0) {
        ULONG tlen = len;
        APTR taddr = CachePreDMA(addr, &tlen, flags);
        if ((flags & DMA_Continue) == 0) {
            flags |= DMA_Continue;
            paddr = (int) taddr;
        }
        len -= tlen;
        addr += tlen;
    }
    return (paddr);
}
#endif

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

void exit(int __status)
{
    KPrintF((CONST_STRPTR) "exit(%ld)", errno);
    while (1)
        ;
}

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
