//
// Copyright 2022-2023 Stefan Reinauer & Chris Hooper
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//

#ifndef _PORT_H
#define _PORT_H

#include <stdint.h>
#include "printf.h"
#include "callout.h"
#include <proto/exec.h>
#include <exec/execbase.h>
#include <inline/exec.h>
#include <exec/memory.h>
#include <clib/exec_protos.h>

typedef uint32_t __attribute__((__may_alias__)) aliased_uint32_t;

#define BIT(x)        (1 << (x))
#define ARRAY_SIZE(x) ((sizeof (x) / sizeof ((x)[0])))
#define ADDR8(x)      (volatile uint8_t *)(x)
#define ADDR32(x)     (volatile aliased_uint32_t *)(x)

#include <sys/param.h>

#define PAGE_SIZE NBPG

#define __KERNEL_RCSID(x,y)
#define UNCONF 1

#define AMIGA_MAX_TRANSFER (1 << 20)  // Maximum DMA size (scatter-gather entry)
#undef MAXPHYS
#define MAXPHYS AMIGA_MAX_TRANSFER

struct device;
typedef struct device *device_t;

void panic(const char *s, ...);
int irq_and_timer_handler(void);

#define __USE(x) (/*LINTED*/(void)(x))

void *device_private(device_t dev);
const char *device_xname(void *dev);

int bsd_splbio(void);
void bsd_splx(int ilevel);

#define hz TICKS_PER_SECOND
#define mstohz(m) ((m) * TICKS_PER_SECOND / 1000)
#define kvtop(x) ((uint32_t)(x))

void delay(int usecs);

#define __UNVOLATILE(x) ((void *)(unsigned long)(volatile void *)(x))
#define __UNCONST(a) ((void *)(intptr_t)(a))

#define B_WRITE         0x00000000      /* Write buffer (pseudo flag). */
#define B_READ          0x00100000      /* Read buffer. */

#define memcpy(dst, src, len) CopyMem(src, dst, len)
// SetMem needs utility.library
// #define memset(dst, value, len) SetMem(dst, value, len)

#define SDT_PROBE1(v,w,x,y,z)
#define SDT_PROBE2(u,v,w,x,y,z)
#define SDT_PROBE3(t,u,v,w,x,y,z)
#define KASSERT(x)

#define uimin(x,y) ((x <= y) ? (x) : (y))
#define mutex_init(x, y, z)
#define mutex_enter(x) do { } while (0)
#define mutex_exit(x)  do { } while (0)
#define cv_init(x, y)
#define cv_wait(x, y)
#define cv_broadcast(x)
#define cv_destroy(x)

#if (!defined(DEBUG_ATTACH)      && \
     !defined(DEBUG_DEVICE)      && \
     !defined(DEBUG_CMDHANDLER)  && \
     !defined(DEBUG_NCR53CXXX)   && \
     !defined(DEBUG_PORT)        && \
     !defined(DEBUG_SCSIPI_BASE) && \
     !defined(DEBUG_SCSICONF)    && \
     !defined(DEBUG_SCSIMSG)     && \
     !defined(DEBUG_SD)          && \
     !defined(DEBUG_SIOP)        && \
     !defined(DEBUG_BOOTMENU)    && \
     !defined(DEBUG_MOUNTER)) || defined(NO_SERIAL_OUTPUT)
#ifdef USE_SERIAL_OUTPUT
#undef USE_SERIAL_OUTPUT
#endif
#endif

#ifndef USE_SERIAL_OUTPUT
#define printf(x...)   do { } while (0)
#define vprintf(x...)  do { } while (0)
#define vfprintf(x...) do { } while (0)
#define putchar(x...)  do { } while (0)
#endif

#ifndef USING_BASEREL
#undef __saveds
#define __saveds
#endif
#endif /* _PORT_H */
