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

void flash_unlock_sdp(void);
void flash_erase_chip(void);
UBYTE flash_readByte(ULONG address);
void flash_writeByte(ULONG address, UBYTE data);
bool flash_init(UBYTE *manuf, UBYTE *devid, volatile UBYTE *base, ULONG *size, ULONG *sectorSize);
void flash_erase_sector(ULONG address, ULONG sectorSize);
void flash_erase_bank(ULONG sectorSize, ULONG bankSize);

#endif
