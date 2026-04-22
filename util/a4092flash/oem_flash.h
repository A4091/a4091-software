/*
 * oem_flash.h - OEM data section for custom boot screen image
 *
 * Stores one or more ZX0-compressed planar bitmap images in SPI flash,
 * displayed on the boot menu screen instead of the vectored card art.
 *
 * Copyright (C) 2026 Stefan Reinauer
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef OEM_FLASH_H
#define OEM_FLASH_H

#include <stdint.h>

#define OEM_FLASH_OFFSET    0x60000
#define OEM_FLASH_SIZE      0x1D000     /* 116 KB (29 x 4KB sectors) */

#define OEM_MAGIC           0x4F454D00  /* "OEM\0" */
#define OEM_VERSION         2
#define OEM_MAX_COLORS      32
#define OEM_VARIANT_SLOTS   3
#define OEM_COORD_CENTER    0xFFFF

#define OEM_VARIANT_DEPTH(slot) (5 - (slot))

/*
 * OEM image bundle - stored at the start of the OEM flash section.
 * Compressed planar bitmap data for the present variants follows immediately
 * after the fixed-size header block.
 *
 * Each variant slot corresponds to a fixed bitplane depth:
 *   slot 0 -> 5 bitplanes
 *   slot 1 -> 4 bitplanes
 *   slot 2 -> 3 bitplanes
 *
 * A variant is considered present when compressed_size is non-zero.
 *
 * Palette always contains 32 entries (64 bytes). Only the first
 * (1 << depth) entries are used; unused entries should be zero.
 * Palette format is Amiga RGB444, big-endian uint16_t per entry.
 */
struct oem_variant {
    uint16_t width;                     /* image width in pixels */
    uint16_t height;                    /* image height in lines */
    uint16_t x;                         /* OEM_COORD_CENTER or absolute X */
    uint16_t y;                         /* OEM_COORD_CENTER or absolute Y */
    uint16_t palette[OEM_MAX_COLORS];   /* Amiga RGB444, big-endian */
    uint32_t compressed_size;           /* ZX0 compressed data bytes */
    uint32_t uncompressed_size;         /* raw planar bitmap bytes */
    uint32_t data_offset;               /* offset from bundle start to payload */
} __attribute__((packed)) __attribute__((aligned(2)));

struct oem_header {
    uint32_t magic;                               /* OEM_MAGIC */
    uint16_t version;                             /* OEM_VERSION */
    uint8_t  variant_count;                       /* number of present variants */
    uint8_t  reserved0;
    uint32_t total_size;                          /* total blob size in bytes */
    struct oem_variant variant[OEM_VARIANT_SLOTS];
    uint32_t checksum;                            /* XOR32 over preceding bytes */
} __attribute__((packed)) __attribute__((aligned(2)));

_Static_assert(sizeof(struct oem_variant) == 84,
               "oem_variant structure must be exactly 84 bytes");
_Static_assert(sizeof(struct oem_header) == 268,
               "oem_header structure must be exactly 268 bytes");

static inline uint32_t oem_bytes_per_row(uint16_t width)
{
    return ((width + 15U) / 16U) * 2U;
}

static inline uint32_t oem_expected_uncompressed_size(const struct oem_variant *v,
                                                      unsigned depth)
{
    return oem_bytes_per_row(v->width) * (uint32_t)v->height * (uint32_t)depth;
}

/* Validate the bundle header before examining individual variants. */
#define OEM_IS_VALID(h) \
    ((h)->magic == OEM_MAGIC && \
     (h)->version == OEM_VERSION && \
     (h)->variant_count <= OEM_VARIANT_SLOTS && \
     (h)->total_size >= sizeof(struct oem_header) && \
     (h)->total_size <= OEM_FLASH_SIZE)

#endif /* OEM_FLASH_H */
