#ifdef DEBUG_DEVICE
#define USE_SERIAL_OUTPUT
#endif
#include "port.h"
#include "printf.h"
#include <stdlib.h>
#include <stdio.h>
#include "device.h"
#include "attach.h"

extern a4091_save_t *asave;

static uint32_t RomFetch32(uint32_t offset)
{
    uint8_t * rombase = (uint8_t *)asave->as_addr;
    uint32_t ret=0;
    int i;
    for (i=0; i<16; i+=2) {
        ret <<=4;
        ret |= rombase[offset*4 +i] >>4;
    }
    return ret;
}

void parse_romfiles(void)
{
    if (RomFetch32((64*1024)-8) == 0xffff5352 &&
        RomFetch32((64*1024)-4) == 0x2f434448) {
        asave->romfile[0]=RomFetch32((64*1024) - 12);
        asave->romfile[1]=RomFetch32((64*1024) - 16);
        printf("ROM signature found: DEV=%08x FS=%08x\n",
                        asave->romfile[0], asave->romfile[1]);
    }
}
