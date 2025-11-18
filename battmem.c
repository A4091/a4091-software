//
// Copyright 2022-2025 Stefan Reinauer
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//

#ifdef DEBUG_DEVICE
#define USE_SERIAL_OUTPUT
#endif
#include "port.h"
#include "printf.h"
#include <stdlib.h>
#include <stdio.h>
#include <resources/battmem.h>
#include <pragmas/exec_pragmas.h>
#include <clib/battmem_protos.h>
#include <pragmas/battmem_pragmas.h>

#include "device.h"
#include "attach.h"
#include "battmem.h"

struct Library *BattMemBase;

int Load_BattMem(void)
{
    UBYTE cdrom_boot = 0,
          ignore_last = 0;
#ifdef ENABLE_QUICKINTS
    UBYTE quick_int = 0;
#endif

    BattMemBase = OpenResource(BATTMEMNAME);
    if (!BattMemBase)
        return 0;

    ObtainBattSemaphore();
    printf("Retrieving settings from BattMem\n");
    ReadBattMem(&cdrom_boot,
                BATTMEM_A4091_CDROM_BOOT_ADDR,
                BATTMEM_A4091_CDROM_BOOT_LEN);
    ReadBattMem(&ignore_last,
                BATTMEM_A4091_IGNORE_LAST_ADDR,
                BATTMEM_A4091_IGNORE_LAST_LEN);
#ifdef ENABLE_QUICKINTS
    ReadBattMem(&quick_int,
                BATTMEM_A4091_QUICK_INT_ADDR,
                BATTMEM_A4091_QUICK_INT_LEN);
#endif

    // CDROM_BOOT defaults to on, hence invert it
    asave->cdrom_boot = !cdrom_boot;
    asave->ignore_last = ignore_last;
#ifdef ENABLE_QUICKINTS
    asave->quick_int = quick_int;
#endif
    printf("  cdrom_boot: %s\n", asave->cdrom_boot?"on":"off");
    printf("  ignore_last: %s\n", asave->ignore_last?"on":"off");
#ifdef ENABLE_QUICKINTS
    printf("  quick_int: %s\n", asave->quick_int?"on":"off");
#endif
    ReleaseBattSemaphore();

    return 1;
}

int Save_BattMem(void)
{
    UBYTE cdrom_boot = !asave->cdrom_boot,
          ignore_last = asave->ignore_last;
#ifdef ENABLE_QUICKINTS
    UBYTE quick_int = asave->quick_int;
#endif

    if (!BattMemBase)
        return 0;

    ObtainBattSemaphore();
    printf("Storing settings to BattMem\n");
    printf("  cdrom_boot: %s\n", asave->cdrom_boot?"on":"off");
    printf("  ignore_last: %s\n", asave->ignore_last?"on":"off");
#ifdef ENABLE_QUICKINTS
    printf("  quick_int: %s\n", asave->quick_int?"on":"off");
#endif
    WriteBattMem(&cdrom_boot,
                 BATTMEM_A4091_CDROM_BOOT_ADDR,
                 BATTMEM_A4091_CDROM_BOOT_LEN);
    WriteBattMem(&ignore_last,
                 BATTMEM_A4091_IGNORE_LAST_ADDR,
                 BATTMEM_A4091_IGNORE_LAST_LEN);
#ifdef ENABLE_QUICKINTS
    WriteBattMem(&quick_int,
                 BATTMEM_A4091_QUICK_INT_ADDR,
                 BATTMEM_A4091_QUICK_INT_LEN);
#endif

    ReleaseBattSemaphore();

    return 1;
}
