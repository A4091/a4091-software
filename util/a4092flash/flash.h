// SPDX-License-Identifier: GPL-2.0-only
/* This file is part of a4092flash
 * Copyright (C) 2023 Matthew Harlum <matt@harlum.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef FLASH_H
#define FLASH_H

#ifdef __AMIGA__
#include <exec/types.h>
#endif
#include <stdbool.h>

/* Flash type enumeration */
typedef enum {
    FLASH_TYPE_NONE = 0,
    FLASH_TYPE_PARALLEL,
    FLASH_TYPE_SPI
} flash_type_t;

/* Get current flash type */
flash_type_t flash_get_type(void);

/* Unified flash API - automatically dispatches to SPI or Parallel implementation */
bool flash_init(UBYTE *manuf, UBYTE *devid, volatile UBYTE *base, ULONG *size, ULONG *sectorSize);
UBYTE flash_readByte(ULONG address);
void flash_writeByte(ULONG address, UBYTE data);
bool flash_erase_chip(void);
bool flash_erase_sector(ULONG address, ULONG sectorSize);
bool flash_erase_bank(ULONG address, ULONG sectorSize, ULONG bankSize);
void flash_cleanup(void);

#ifdef FLASH_PARALLEL
/* Parallel flash specific functions (used internally) */
void parallel_flash_unlock_sdp(void);
bool parallel_flash_init(UBYTE *manuf, UBYTE *devid, volatile UBYTE *base, ULONG *size, ULONG *sectorSize);
UBYTE parallel_flash_readByte(ULONG address);
void parallel_flash_writeByte(ULONG address, UBYTE data);
bool parallel_flash_erase_chip(void);
bool parallel_flash_erase_sector(ULONG address, ULONG sectorSize);
bool parallel_flash_erase_bank(ULONG address, ULONG sectorSize, ULONG bankSize);
#endif

#endif
