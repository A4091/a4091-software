/*
 * mfg_data.h - Manufacturing data structure for A4092/A4770 SCSI controllers
 *
 * This structure is stored in SPI flash and contains factory-programmed
 * information for production tracking, component traceability, and
 * field support.
 *
 * Flash layout (512 KB W25X40):
 *   0x00000 - 0x7CFFF  (500 KB)  Firmware/ROM Image
 *   0x7D000 - 0x7EFFF  (8 KB)    NVRAM/Settings
 *   0x7F000 - 0x7FFFF  (4 KB)    Manufacturing Data
 *
 * Copyright (C) 2025 Stefan Reinauer
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef MFG_DATA_H
#define MFG_DATA_H

#include <stdint.h>

#define MFG_FLASH_OFFSET    0x7F000
#define MFG_FLASH_SIZE      0x1000      /* 4 KB sector */

#define MFG_MAGIC           0x4D464741  /* "MFGA" - ManuFacturinG Amiga */
#define MFG_VERSION         1

/* PCB color helper macros - RGB444 format */
#define PCB_COLOR_RG(r, g)      (((r) << 4) | ((g) & 0x0F))
#define PCB_COLOR_B(b)          ((b) & 0x0F)
#define PCB_COLOR_R(mfg)        (((mfg)->pcb_color_rg >> 4) & 0x0F)
#define PCB_COLOR_G(mfg)        ((mfg)->pcb_color_rg & 0x0F)
#define PCB_COLOR_GET_B(mfg)    ((mfg)->pcb_color_b & 0x0F)

/* Convert RGB444 to RGB888 for display */
#define PCB_COLOR_R8(mfg)       (PCB_COLOR_R(mfg) * 17)
#define PCB_COLOR_G8(mfg)       (PCB_COLOR_G(mfg) * 17)
#define PCB_COLOR_B8(mfg)       (PCB_COLOR_GET_B(mfg) * 17)

/* Factory test status bitmask */
#define TEST_PASSED_REGS    (1 << 0)    /* Register read/write test */
#define TEST_PASSED_IRQ     (1 << 1)    /* Interrupt test */
#define TEST_PASSED_SCSI    (1 << 2)    /* SCSI bus test */
#define TEST_PASSED_DMA     (1 << 3)    /* DMA test */
#define TEST_PASSED_FLASH   (1 << 4)    /* SPI flash test */
#define TEST_PASSED_CPLD    (1 << 5)    /* CPLD functionality test */
#define TEST_PASSED_ALL     0x003F      /* All tests passed */

/*
 * Manufacturing data structure - 256 bytes total
 *
 * This structure is written once at the factory and should not be
 * modified in the field, except for the owner_name field which
 * can be updated by the end user.
 */
struct mfg_data {
    /* Header */
    uint32_t magic;                     /* MFG_MAGIC */
    uint16_t struct_version;            /* MFG_VERSION - for future expansion */
    uint16_t reserved0;

    /* Identity */
    char     card_type[8];              /* "A4092", "A4770", etc. null-term */
    char     serial[16];                /* e.g. "A4092-0001" null-terminated */
    uint16_t hw_revision;               /* 0x0100 = rev 1.0, 0x0201 = rev 2.1 */
    uint8_t  pcb_color_rg;              /* (R << 4) | G, 0 = use default */
    uint8_t  pcb_color_b;               /* B in lower 4 bits, 0 = use default */

    /* Production - who built it */
    char     assembler_name[32];        /* e.g. "Stefan Reinauer" */
    char     assembly_factory[32];      /* e.g. "JLCPCB" */
    uint32_t build_date;                /* Unix timestamp */
    uint16_t batch_number;              /* Lot/batch for component traceability */
    uint16_t reserved2;

    /* Components */
    char     siop_datecode[8];          /* 53c710/53c770 date code from chip */
    uint16_t cpld_version;              /* CPLD bitstream version */
    uint16_t initial_fw_version;        /* FW flashed at factory (4235 = 42.35) */

    /* Factory Test */
    uint32_t test_date;                 /* Unix timestamp */
    uint16_t test_fw_version;           /* Test firmware version */
    uint16_t test_status;               /* Bitmask of TEST_PASSED_* flags */

    /* User-writable */
    char     owner_name[32];            /* Optional: owner can personalize */

    /* Product */
    char     sku[32];                   /* e.g. "A4092 Black Edition" null-term */

    /* Reserved for future expansion */
    uint8_t  reserved[60];

    /* Integrity check - CRC32 over all preceding bytes (252 bytes) */
    uint32_t crc32;
} __attribute__((packed));

/* Verify structure size at compile time */
_Static_assert(sizeof(struct mfg_data) == 256,
               "mfg_data structure must be exactly 256 bytes");

/*
 * Helper macros
 */

/* Check if manufacturing data is valid */
#define MFG_IS_VALID(mfg) \
    ((mfg)->magic == MFG_MAGIC && (mfg)->struct_version <= MFG_VERSION)

/* Check if all factory tests passed */
#define MFG_TESTS_PASSED(mfg) \
    (((mfg)->test_status & TEST_PASSED_ALL) == TEST_PASSED_ALL)

/* Extract major/minor from hw_revision (e.g., 0x0201 -> 2.1) */
#define MFG_HW_REV_MAJOR(mfg)   (((mfg)->hw_revision >> 8) & 0xFF)
#define MFG_HW_REV_MINOR(mfg)   ((mfg)->hw_revision & 0xFF)

#endif /* MFG_DATA_H */
