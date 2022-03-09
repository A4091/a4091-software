#ifndef _PORT_H
#define _PORT_H

// #include <stdio.h>
#include <stdint.h>
#include "printf.h"
// #include <stdarg.h>
#include <clib/debug_protos.h>

#define BIT(x)        (1 << (x))
#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))

#undef __RCSID
#define __RCSID(x)
#define __KERNEL_RCSID(x,y)

#include <sys/param.h>
#define PAGE_SIZE NBPG
// #define MAXPHYS   (1 << 20)  // Maximum physical DMA size

struct device;
typedef struct device *device_t;

void panic(const char *s);

#define __USE(x) (/*LINTED*/(void)(x))

// void *device_private(void *ptr);
const char *device_xname(void *dev);

int bsd_splbio();
void bsd_splx();

typedef uint32_t paddr_t;
typedef uint32_t vaddr_t;
int mstohz(int m);
uint32_t kvtop(void *vaddr);

int dma_cachectl(void *addr, int len);
void callout_reset(void *cs, int to_ticks, void (*func)(void *), void *arg);
int callout_stop(void *cs);
void delay(int usecs);

#define __UNVOLATILE(x) ((void *)(unsigned long)(volatile void *)(x))
#define __UNCONST(a) ((void *)(intptr_t)(a))

void _DCIAS(uint32_t paddr);

#define B_WRITE         0x00000000      /* Write buffer (pseudo flag). */
#define B_READ          0x00100000      /* Read buffer. */

// Can't assign printf() to KPrintF() because RawDoFmt() assumes 16-bit ints
#if 0
#undef printf
#define printf dbgprintf
int dbgprintf(const char *fmt, ...);
#endif

// void *local_memcpy(void *dest, const void *src, size_t n);
// void *local_memset(void *dest, int value, size_t len);
// #define memcpy local_memcpy
// #define memset local_memset
// #define memcpy(dst, src, len) CopyMem(src, dst, len)
// #define memset(dst, value, len) SetMem(dst, value, len)

#define SDT_PROBE1(v,w,x,y,z)
#define SDT_PROBE3(t,u,v,w,x,y,z)
#define KASSERT(x)

#define mutex_enter(x)
#define mutex_exit(x)

#endif /* _PORT_H */
