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

#include <stdio.h>
#include <proto/expansion.h>
#include <exec/types.h>
#include <stdbool.h>

#include "flash.h"
#include "flash_constants.h"

static ULONG flashbase;

/**
 * @brief Reads a single byte from the flash memory at the given byte address,
 * applying the custom 32-bit word mapping.
 * The mapping assumes:
 * - Each 32-bit word at (flashbase + byte_address * 4) corresponds to a single byte.
 * - Bits 28-31 of the 32-bit word contain the higher nibble of the byte.
 * - Bits 12-15 of the 32-bit word contain the lower nibble of the byte.
 *
 * @param byte_address The byte address (0x000000 - 0x7FFFFF) to read from.
 * @return The UBYTE value read from the flash.
 */
static UBYTE flash_read_byte(ULONG byte_address)
{
    // Calculate the physical 32-bit word address in memory.
    // Each byte address maps to a unique 32-bit word in the underlying memory.
    volatile ULONG *word_ptr = (volatile ULONG *)(flashbase + (byte_address * 4));

    // Read the full 32-bit word value from the memory-mapped flash.
    ULONG word_value = *word_ptr;

    // Extract the higher nibble (bits 28-31) and shift it into the correct position
    // (most significant 4 bits of the UBYTE).
    UBYTE high_nibble = (UBYTE)((word_value >> 28) & 0x0F);

    // Extract the lower nibble (bits 12-15) and keep it in its correct position
    // (least significant 4 bits of the UBYTE).
    UBYTE low_nibble = (UBYTE)((word_value >> 12) & 0x0F);

    // Combine the two nibbles to form the complete 8-bit byte.
    return (high_nibble << 4) | low_nibble;
}

/**
 * @brief Writes a single byte to the flash memory at the given byte address,
 * applying the custom 32-bit word mapping.
 * The mapping assumes:
 * - Each 32-bit word at (flashbase + byte_address * 4) corresponds to a single byte.
 * - The higher nibble of the byte should be written to bits 28-31 of the 32-bit word.
 * - The lower nibble of the byte should be written to bits 12-15 of the 32-bit word.
 *
 * @param byte_address The byte address (0x000000 - 0x7FFFFF) to write to.
 * @param data The UBYTE value to write to the flash.
 */
static void flash_write_byte(ULONG byte_address, UBYTE data)
{
    // Calculate the physical 32-bit word address in memory.
    // Each byte address maps to a unique 32-bit word in the underlying memory.
    volatile ULONG *word_ptr = (volatile ULONG *)(flashbase + (byte_address * 4));

    // Extract the higher 4 bits (nibble) and lower 4 bits (nibble) from the data byte.
    UBYTE high_nibble_to_write = (data >> 4) & 0x0F;
    UBYTE low_nibble_to_write = data & 0x0F;

    // Prepare the new high nibble value, shifting it to bit positions 28-31.
    ULONG new_high_nibble_part = ((ULONG)high_nibble_to_write) << 28;
    // Prepare the new low nibble value, shifting it to bit positions 12-15.
    ULONG new_low_nibble_part = ((ULONG)low_nibble_to_write) << 12;

    // Combine the preserved bits with the newly prepared high and low nibble parts.
    ULONG new_word_value = new_high_nibble_part | new_low_nibble_part;

    // Write the modified 32-bit word back to the memory-mapped flash.
    *word_ptr = new_word_value;
}

static inline void flash_command(UBYTE);
static inline void flash_poll(ULONG);

struct flashchips {
	UWORD id;
	UWORD size;
	const char *vendor;
	const char *device;
};

static const struct flashchips devices_supported[] = {
    { 0x0120, 128, "AMD",      "AM29F010"       },
    { 0x01A4, 512, "AMD",      "AM29F040"       },
    { 0xBFB5, 128, "SST",      "SST39SF010"     },
    { 0xBFB6, 256, "SST",      "SST39SF020"     },
    { 0xBFB7, 512, "SST",      "SST39SF040"     },
    { 0xC2A4, 512, "Macronix", "MX29F040C"      },
    { 0x0000,   0, NULL,       NULL             }
};

/** flash_is_supported
 *
 * @brief Check if the device id is supported
 * @param manufacturer the manufacturer ID
 * @param device the device id
 * @returns boolean result
 */
static bool flash_is_supported(UBYTE manufacturer, UBYTE device, UWORD *size)
{
  ULONG deviceId = (manufacturer << 8) | device;
  int i = 0;

  if (size) *size = 0;

  while (devices_supported[i].id != 0) {
    if (devices_supported[i].id == deviceId) {
      printf("Flash part: %s %s (%d KB)\n",
		      devices_supported[i].vendor,
		      devices_supported[i].device,
		      devices_supported[i].size);
      *size = devices_supported[i].size;
      return true;
    }

    i++;
  }

  return false;
}

/** flash_get_sectorSize
 *
 * @brief return the sector size for this device type if known
 * @param manufacturer the manufacturer ID
 * @param device the device id
 * @returns UWORD sector size in bytes
 */
static UWORD flash_get_sectorSize(UBYTE manufacturer, UBYTE device)
{
    ULONG deviceId = (manufacturer << 8) | device;
    UWORD sectorSize;

    switch (deviceId) {
      case 0xBFB5: // SST39SF010
      case 0xBFB6: // SST39SF020
      case 0xBFB7: // SST39SF040
        sectorSize = 4096;
        break;
      case 0x0120: // AM29F010
        sectorSize = 16384;
        break;
      default:
        // Unknown/Unsupported/Too large
        // If the device's sectorSize is greater than 32K don't bother
        sectorSize = 0;
    }

    return sectorSize;
}

/** flash_readByte
 *
 * @brief Read a byte from Flash
 * @param address Address to read from
 * @return The data that was read
 */
UBYTE flash_readByte(ULONG address)
{
  // Mask address to ensure it is within the valid flash size.
  address &= (FLASH_SIZE - 1);

  return flash_read_byte(address);
}



/** flash_writeByte
 *
 * @brief Write a byte to the Flash
 * @param address Address to write to
 * @param data The data to be written
 */
void flash_writeByte(ULONG address, UBYTE data)
{
  // Mask address to ensure it is within the valid flash size.
  address &= (FLASH_SIZE - 1);

  flash_unlock_sdp();
  flash_command(CMD_BYTE_PROGRAM);
  flash_write_byte(address, data);
  flash_poll(address); // Poll the status using the byte address
  return;
}

/** flash_command
 *
 * @brief send a command to the Flash
 * @param command The command byte to send.
 */
static inline void flash_command(UBYTE command)
{
  // Write command byte to the specific command address
  flash_write_byte(ADDR_CMD_STEP_1, command);
  return;
}

/** flash_unlock_sdp
 *
 * @brief Send the SDP command sequence
 */
void flash_unlock_sdp(void)
{
  // Write the sequence bytes to the specific addresses
  flash_write_byte(ADDR_CMD_STEP_1, CMD_SDP_STEP_1);
  flash_write_byte(ADDR_CMD_STEP_2, CMD_SDP_STEP_2);
  return;
}

/** flash_erase_chip
 *
 * @brief Perform a chip erase.
 */
void flash_erase_chip(void)
{
  flash_unlock_sdp();
  flash_command(CMD_ERASE);
  flash_unlock_sdp();
  flash_command(CMD_ERASE_CHIP);

  flash_poll(0);
}

/** flash_erase_bank
 *
 * Erase the currently selected 32KB bank
 *
 */
void flash_erase_bank(UWORD sectorSize)
{
  if (sectorSize > 0) {
    int count = 32768 / sectorSize;
    for (int i = 0; i < count; i++) {
      flash_erase_sector(i * sectorSize);
    }
  }
}

/** flash_erase_sector
 *
 * @brief Erase a sector
 * @param address Address of sector to erase
 *
 */
void flash_erase_sector(ULONG address)
{
  // Mask address to ensure it is within the valid flash size.
  address &= (FLASH_SIZE - 1);

  flash_unlock_sdp();
  flash_command(CMD_ERASE);
  flash_unlock_sdp();
  // Write erase sector command to the specific sector address
  flash_write_byte(address, CMD_ERASE_SECTOR);
  flash_poll(address);
}

/** flash_poll
 *
 * @brief Poll the status bits at address, until they indicate that the operation has completed.
 * @param address Address to poll
 */
static inline void flash_poll(ULONG address)
{
  // Mask address to ensure it is within the valid flash size.
  address &= (FLASH_SIZE - 1);

  UBYTE val1, val2;
  // Continuously read the status byte twice until the status bit 6 (DQ6) matches,
  // indicating the operation has completed.
  do {
    val1 = flash_read_byte(address);
    val2 = flash_read_byte(address);
  } while (((val1 & (1 << 6)) != (val2 & (1 << 6))));
}

/** flash_init
 *
 * @brief Check the manufacturer id of the device, return manuf and dev id
 * @param manuf Pointer to a UBYTE that will be updated with the returned manufacturer id
 * @param devid Pointer to a UBYTE that will be updatet with the returned device id
 * @param flashbase Pointer to the Flash base address
 * @return True if the manufacturer ID matches the expected value and flashbase is valid.
 */
bool flash_init(UBYTE *manuf, UBYTE *devid, volatile UBYTE *base, ULONG *size, UWORD *sectorSize)
{
  bool ret = false;
  UBYTE manufId;
  UBYTE deviceId;

  if (size) *size = 0;
  if (sectorSize) *sectorSize = 0;
 
  // Set the global flashbase pointer.
  flashbase = (ULONG)base;

  flash_unlock_sdp();
  flash_command(CMD_ID_ENTRY);

  // Read manufacturer ID
  manufId = flash_read_byte(0);
  // Read device ID
  deviceId = flash_read_byte(1);

  flash_command(CMD_CFI_ID_EXIT);

  // Update the output parameters if pointers are valid.
  if (manuf) *manuf = manufId;
  if (devid) *devid = deviceId;

  // Check if the device is supported and flashbase is valid.
  UWORD flash_size;
  if (flash_is_supported(manufId, deviceId, &flash_size) && flashbase) {
    // Update size if pointer is valid.
    if (size) *size = flash_size * 1024;
    // Update the sector size if pointer is valid.
    if (sectorSize) *sectorSize = flash_get_sectorSize(manufId, deviceId);
    ret = true;
  }

  return (ret);
}
