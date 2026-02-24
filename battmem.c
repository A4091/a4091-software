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
#if defined(FLASH_PARALLEL) || defined(FLASH_SPI)
#include "util/a4092flash/nvram_flash.h"
#endif

struct Library *BattMemBase;

int Load_BattMem(void)
{
#if defined(FLASH_PARALLEL) || defined(FLASH_SPI)
    /* A4092: Read settings from NVRAM flash */
    int res = flash_read_nvram(NVRAM_OFFSET, &asave->nvram.nv);
    if (res == NVRAM_OK) {
        UBYTE osf = asave->nvram.nv.settings.os_flags;
        asave->cdrom_boot  = (osf & BIT(0)) ? 1 : 0;
        asave->ignore_last = (osf & BIT(1)) ? 1 : 0;
#ifdef ENABLE_QUICKINTS
        asave->quick_int   = (osf & BIT(2)) ? 1 : 0;
#endif
        asave->allow_disc  = (osf & BIT(3)) ? 1 : 0;
        /* Default Amiga blue — mfg_read() overrides if mfg data is valid */
        asave->menu_color_r = 6;
        asave->menu_color_g = 8;
        asave->menu_color_b = 11;
        asave->nvram.os_dirty = 0;
        asave->nvram.switch_dirty = 0;
        asave->nvram.color_dirty = 0;
        printf("Retrieving settings from NVRAM flash\n");
    } else {
        /* Defaults if no valid entry or partition: cdrom boot ON, others OFF */
        asave->cdrom_boot  = 1;
        asave->ignore_last = 0;
#ifdef ENABLE_QUICKINTS
        asave->quick_int   = 0;
#endif
        asave->allow_disc  = 0;
        /* Default Amiga blue */
        asave->menu_color_r = 6;
        asave->menu_color_g = 8;
        asave->menu_color_b = 11;
        asave->nvram.os_dirty = 0;
        asave->nvram.switch_dirty = 0;
        asave->nvram.color_dirty = 0;
        printf("NVRAM not initialized (res=%d). Using defaults.\n", res);
    }
    printf("  cdrom_boot: %s\n", asave->cdrom_boot?"on":"off");
    printf("  ignore_last: %s\n", asave->ignore_last?"on":"off");
#ifdef ENABLE_QUICKINTS
    printf("  quick_int: %s\n", asave->quick_int?"on":"off");
#endif
    printf("  allow_disc: %s\n", asave->allow_disc?"on":"off");
    return 1;
#else
    UBYTE cdrom_boot = 0,
          ignore_last = 0,
          allow_disc = 0;
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
    ReadBattMem(&allow_disc,
                BATTMEM_A4091_ALLOW_DISC_ADDR,
                BATTMEM_A4091_ALLOW_DISC_LEN);

    // CDROM_BOOT defaults to on, hence invert it
    asave->cdrom_boot = !cdrom_boot;
    asave->ignore_last = ignore_last;
#ifdef ENABLE_QUICKINTS
    asave->quick_int = quick_int;
#endif
    asave->allow_disc = allow_disc;
    printf("  cdrom_boot: %s\n", asave->cdrom_boot?"on":"off");
    printf("  ignore_last: %s\n", asave->ignore_last?"on":"off");
#ifdef ENABLE_QUICKINTS
    printf("  quick_int: %s\n", asave->quick_int?"on":"off");
#endif
    printf("  allow_disc: %s\n", asave->allow_disc?"on":"off");
    ReleaseBattSemaphore();

    return 1;
#endif
}

int Save_BattMem(void)
{
#if defined(FLASH_PARALLEL) || defined(FLASH_SPI)
    /* Stage A4092 settings to NVRAM cache, defer flash write */
    UBYTE osf = 0;
    if (asave->cdrom_boot)  osf |= BIT(0);
    if (asave->ignore_last) osf |= BIT(1);
#ifdef ENABLE_QUICKINTS
    if (asave->quick_int)   osf |= BIT(2);
#endif
    if (asave->allow_disc)  osf |= BIT(3);
    asave->nvram.nv.settings.os_flags = osf;
    asave->nvram.os_dirty = 1;
    printf("Staging settings to NVRAM cache\n");
    printf("  cdrom_boot: %s\n", asave->cdrom_boot?"on":"off");
    printf("  ignore_last: %s\n", asave->ignore_last?"on":"off");
#ifdef ENABLE_QUICKINTS
    printf("  quick_int: %s\n", asave->quick_int?"on":"off");
#endif
    printf("  allow_disc: %s\n", asave->allow_disc?"on":"off");
    return 1;
#else
    UBYTE cdrom_boot = !asave->cdrom_boot,
          ignore_last = asave->ignore_last,
          allow_disc = asave->allow_disc;
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
    printf("  allow_disc: %s\n", asave->allow_disc?"on":"off");
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
    WriteBattMem(&allow_disc,
                 BATTMEM_A4091_ALLOW_DISC_ADDR,
                 BATTMEM_A4091_ALLOW_DISC_LEN);

    ReleaseBattSemaphore();

    return 1;
#endif
}

#if defined(FLASH_PARALLEL) || defined(FLASH_SPI)
int Nvram_CommitDirty(void)
{
    if (!asave)
        return 0;
    if (!asave->nvram.os_dirty && !asave->nvram.switch_dirty)
        return 0;

    /* Persist current cached values to flash */
    printf("Committing settings to NVRAM flash...\n");
    int res = flash_write_nvram(NVRAM_OFFSET, &asave->nvram.nv);
    if (res == NVRAM_ERR_BAD_MAGIC) {
        /* Try to initialize partition and retry */
        printf("NVRAM partition missing. Initializing...\n");
        int fmt = flash_format_nvram_partition(NVRAM_OFFSET, NVRAM_SIZE);
        if (fmt == NVRAM_OK) {
            res = flash_write_nvram(NVRAM_OFFSET, &asave->nvram.nv);
        } else {
            printf("NVRAM format failed: %d\n", fmt);
            return -1;
        }
    }
    if (res == NVRAM_OK) {
        asave->nvram.os_dirty = 0;
        asave->nvram.switch_dirty = 0;
        asave->nvram.color_dirty = 0;
        printf("NVRAM settings saved.\n");
        return 1;
    }
    printf("NVRAM write failed: %d\n", res);
    return -1;
}
#endif
