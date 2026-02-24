//
// Copyright 2026 Stefan Reinauer
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

#include "port.h"
#include "printf.h"
#include <string.h>

#include "device.h"
#include "attach.h"
#include "mfg.h"
#include "util/a4092flash/mfg_flash.h"
#include "util/a4092flash/flash.h"

#if defined(FLASH_PARALLEL) || defined(FLASH_SPI)

bool mfg_read(void)
{
    struct mfg_data mfg;
    uint8_t *p = (uint8_t *)&mfg;
    uint32_t i;

    /* Read 256 bytes from manufacturing data sector */
    for (i = 0; i < sizeof (mfg); i++)
        p[i] = flash_readByte(MFG_FLASH_OFFSET + i);

    /* Validate magic */
    if (mfg.magic != MFG_MAGIC) {
        printf("MFG: No manufacturing data found\n");
        return false;
    }

    /* Verify XOR32 checksum: XOR all 64 uint32 words, result must be 0 */
    uint32_t *words = (uint32_t *)&mfg;
    uint32_t xor = 0;
    for (i = 0; i < sizeof (mfg) / sizeof (uint32_t); i++)
        xor ^= words[i];
    if (xor != 0)
        printf("MFG: Warning: checksum mismatch\n");

    /* Extract color: 0,0 means use default Amiga blue */
    if (mfg.pcb_color_rg == 0 && mfg.pcb_color_b == 0) {
        asave->menu_color_r = 6;
        asave->menu_color_g = 8;
        asave->menu_color_b = 11;
    } else {
        asave->menu_color_r = PCB_COLOR_R(&mfg);
        asave->menu_color_g = PCB_COLOR_G(&mfg);
        asave->menu_color_b = PCB_COLOR_GET_B(&mfg);
    }

    /* Copy serial number */
    memcpy(asave->mfg_serial, mfg.serial, sizeof (asave->mfg_serial));
    asave->mfg_serial[sizeof (asave->mfg_serial) - 1] = '\0';

    printf("MFG: Serial %s, HW rev %d.%d\n",
           asave->mfg_serial,
           MFG_HW_REV_MAJOR(&mfg),
           MFG_HW_REV_MINOR(&mfg));

    return true;
}

#endif
