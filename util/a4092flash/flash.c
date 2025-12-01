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

#ifdef _KERNEL
#ifdef DEBUG_FLASH
#define USE_SERIAL_OUTPUT
#endif
#include "port.h"
#include "printf.h"
#endif
#include <stdio.h>
#include <proto/expansion.h>
#include <exec/types.h>
#include <stdbool.h>

#include "flash.h"
#ifdef FLASH_PARALLEL
#include "flash_constants.h"
#endif
#ifdef FLASH_SPI
#include "spi.h"
#endif
#include "nibble_word.h"

#ifdef FLASH_PARALLEL
static ULONG flashbase;
#endif
static ULONG flash_size = 0;
static ULONG sector_size = 0;
static flash_type_t current_flash_type = FLASH_TYPE_NONE;

#ifdef FLASH_PARALLEL
/* ===== Parallel flash implementation ===== */

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
static UBYTE parallel_flash_read_byte(ULONG byte_address)
{
    // Calculate the physical 32-bit word address in memory.
    // Each byte address maps to a unique 32-bit word in the underlying memory.
    volatile ULONG *word_ptr = (volatile ULONG *)(flashbase + (byte_address * 4));

    // Read the full 32-bit word value from the memory-mapped flash.
    ULONG word_value = *word_ptr;

    // Unpack the byte from the A4092 nibble word format
    return unpack_nibble_word(word_value);
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
static void parallel_flash_write_byte(ULONG byte_address, UBYTE data)
{
    // Calculate the physical 32-bit word address in memory.
    // Each byte address maps to a unique 32-bit word in the underlying memory.
    volatile ULONG *word_ptr = (volatile ULONG *)(flashbase + (byte_address * 4));

    // Pack the byte into the A4092 nibble word format and write it
    *word_ptr = pack_nibble_word(data);
}

static inline void parallel_flash_command(UBYTE);
static inline bool parallel_flash_poll(ULONG);

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
 * @returns ULONG sector size in bytes
 */
static ULONG flash_get_sectorSize(UBYTE manufacturer, UBYTE device)
{
    ULONG deviceId = (manufacturer << 8) | device;
    ULONG sectorSize;

    switch (deviceId) {
      case 0xBFB5: // SST39SF010
      case 0xBFB6: // SST39SF020
      case 0xBFB7: // SST39SF040
        sectorSize = 4096;
        break;
      case 0x0120: // AM29F010
        sectorSize = 16384;
        break;
      case 0x01A4: // AM29F040
      case 0xC2A4: // MX29F040C
        sectorSize = 65536;
	break;
      default:
        // Unknown/Unsupported/Too large
        // If the device's sectorSize is greater than 64K don't bother
        sectorSize = 0;
    }

    return sectorSize;
}

/** parallel_flash_readByte
 *
 * @brief Read a byte from parallel Flash
 * @param address Address to read from
 * @return The data that was read
 */
UBYTE parallel_flash_readByte(ULONG address)
{
  // Mask address to ensure it is within the valid flash size.
  address &= (FLASH_SIZE - 1);

  return parallel_flash_read_byte(address);
}



/** parallel_flash_writeByte
 *
 * @brief Write a byte to the parallel Flash
 * @param address Address to write to
 * @param data The data to be written
 */
void parallel_flash_writeByte(ULONG address, UBYTE data)
{
  // Mask address to ensure it is within the valid flash size.
  address &= (FLASH_SIZE - 1);

  parallel_flash_unlock_sdp();
  parallel_flash_command(CMD_BYTE_PROGRAM);
  parallel_flash_write_byte(address, data);
  if (!parallel_flash_poll(address)) {
      printf("Write failed at address 0x%x\n", address);
  }
  return;
}

/** parallel_flash_command
 *
 * @brief send a command to the parallel Flash
 * @param command The command byte to send.
 */
static inline void parallel_flash_command(UBYTE command)
{
  // Write command byte to the specific command address
  parallel_flash_write_byte(ADDR_CMD_STEP_1, command);
  return;
}

/** parallel_flash_unlock_sdp
 *
 * @brief Send the SDP command sequence
 */
void parallel_flash_unlock_sdp(void)
{
  // Write the sequence bytes to the specific addresses
  parallel_flash_write_byte(ADDR_CMD_STEP_1, CMD_SDP_STEP_1);
  parallel_flash_write_byte(ADDR_CMD_STEP_2, CMD_SDP_STEP_2);
  return;
}

/** parallel_flash_erase_chip
 *
 * @brief Perform a chip erase.
 * @return true if successful, false on failure
 */
bool parallel_flash_erase_chip(void)
{
  parallel_flash_unlock_sdp();
  parallel_flash_command(CMD_ERASE);
  parallel_flash_unlock_sdp();
  parallel_flash_command(CMD_ERASE_CHIP);

  return parallel_flash_poll(0);
}

/** parallel_flash_erase_bank
 *
 * Erase a bank starting at the specified address
 *
 * @param address Starting address of the bank
 * @param sectorSize Size of each sector
 * @param bankSize Total size of the bank to erase
 * @return true on success
 */
bool parallel_flash_erase_bank(ULONG address, ULONG sectorSize, ULONG bankSize)
{
  if (sectorSize > 0) {
    int count = bankSize / sectorSize;
    for (int i = 0; i < count; i++) {
      parallel_flash_erase_sector(address + (i * sectorSize), sectorSize);
    }
  }
  return true;
}

/** parallel_flash_erase_sector
 *
 * @brief Erase a sector
 * @param address Address of sector to erase
 * @param sectorSize Size of sector to erase
 * @return true on success
 */
bool parallel_flash_erase_sector(ULONG address, ULONG sectorSize)
{
  (void)sectorSize;  // Unused for parallel flash
  // Mask address to ensure it is within the valid flash size.
  address &= (FLASH_SIZE - 1);

  parallel_flash_unlock_sdp();
  parallel_flash_command(CMD_ERASE);
  parallel_flash_unlock_sdp();
  // Write erase sector command to the specific sector address
  parallel_flash_write_byte(address, CMD_ERASE_SECTOR);
  return parallel_flash_poll(address);
}

/** parallel_flash_poll
 *
 * @brief Poll the status bits at address, until they indicate that the operation has completed.
 * @param address Address to poll
 * @return true if successful, false on timeout
 */
static inline bool parallel_flash_poll(ULONG address)
{
  // Mask address to ensure it is within the valid flash size.
  address &= (FLASH_SIZE - 1);

  UBYTE val1, val2;
  ULONG timeout = 10000000; // Safety timeout

  // Continuously read the status byte twice until the status bit 6 (DQ6) matches,
  // indicating the operation has completed.
  do {
    val1 = parallel_flash_read_byte(address);
    val2 = parallel_flash_read_byte(address);
    timeout--;
  } while ( ((val1 & (1 << 6)) != (val2 & (1 << 6))) && timeout > 0);

  if (timeout == 0) {
     printf("Flash poll timeout at 0x%x!\n", address);
     return false;
  }

  return true;
}

/** parallel_flash_init
 *
 * @brief Check the manufacturer id of the device, return manuf and dev id
 * @param manuf Pointer to a UBYTE that will be updated with the returned manufacturer id
 * @param devid Pointer to a UBYTE that will be updatet with the returned device id
 * @param base Pointer to the Flash base address
 * @return True if the manufacturer ID matches the expected value and flashbase is valid.
 */
bool parallel_flash_init(UBYTE *manuf, UBYTE *devid, volatile UBYTE *base, ULONG *size, ULONG *sectorSize)
{
  bool ret = false;
  UBYTE manufId;
  UBYTE deviceId;

  if (size) *size = 0;
  if (sectorSize) *sectorSize = 0;

  // Set the global flashbase pointer.
  flashbase = (ULONG)base;

  parallel_flash_unlock_sdp();
  parallel_flash_command(CMD_ID_ENTRY);

  // Read manufacturer ID
  manufId = parallel_flash_read_byte(0);
  // Read device ID
  deviceId = parallel_flash_read_byte(1);

  parallel_flash_command(CMD_CFI_ID_EXIT);

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
#endif /* FLASH_PARALLEL */

/* ===== Abstraction layer - unified flash API ===== */

/**
 * flash_get_type
 *
 * @brief Get the current flash type
 * @return The flash type (FLASH_TYPE_NONE, FLASH_TYPE_PARALLEL, or FLASH_TYPE_SPI)
 */
flash_type_t flash_get_type(void)
{
  return current_flash_type;
}

/**
 * flash_init
 *
 * @brief Initialize flash - tries SPI first, then falls back to parallel
 * @param manuf Pointer to store manufacturer ID
 * @param devid Pointer to store device ID
 * @param base Base address of the board
 * @param size Pointer to store flash size
 * @param sectorSize Pointer to store sector size
 * @return true if flash detected and initialized
 */
bool flash_init(UBYTE *manuf, UBYTE *devid, volatile UBYTE *base, ULONG *size, ULONG *sectorSize)
{
#ifdef FLASH_SPI
  // Try SPI first (more common on modern A4092 boards)
  if (spi_flash_init(manuf, devid, base, size, sectorSize)) {
    current_flash_type = FLASH_TYPE_SPI;
    if (size) flash_size = *size;
    if (sectorSize) sector_size = *sectorSize;
    return true;
  }
#endif

#ifdef FLASH_PARALLEL
  // Fall back to parallel flash
  if (parallel_flash_init(manuf, devid, base, size, sectorSize)) {
    current_flash_type = FLASH_TYPE_PARALLEL;
    if (size) flash_size = *size;
    if (sectorSize) sector_size = *sectorSize;
    return true;
  }
#endif

  // No flash detected
  current_flash_type = FLASH_TYPE_NONE;
  flash_size = 0;
  sector_size = 0;
  return false;
}

/**
 * flash_readByte
 *
 * @brief Read a byte from flash (dispatches to SPI or parallel)
 * @param address Address to read from
 * @return The byte read
 */
UBYTE flash_readByte(ULONG address)
{
#ifdef FLASH_SPI
  if (current_flash_type == FLASH_TYPE_SPI) {
    return spi_flash_readByte(address);
  }
#endif
#ifdef FLASH_PARALLEL
  if (current_flash_type == FLASH_TYPE_PARALLEL) {
    return parallel_flash_readByte(address);
  }
#endif
  return 0xFF; // Default to erased state if no flash type available
}

/**
 * flash_writeByte
 *
 * @brief Write a byte to flash (dispatches to SPI or parallel)
 * @param address Address to write to
 * @param data Byte to write
 */
void flash_writeByte(ULONG address, UBYTE data)
{
#ifdef FLASH_SPI
  if (current_flash_type == FLASH_TYPE_SPI) {
    spi_flash_writeByte(address, data);
  }
#endif
#ifdef FLASH_PARALLEL
  if (current_flash_type == FLASH_TYPE_PARALLEL) {
    parallel_flash_writeByte(address, data);
  }
#endif
}

/**
 * flash_verify_erased
 *
 * @brief Verify that a flash region contains only 0xFF bytes
 * @param address Starting address
 * @param size Size of region to verify
 * @return true if all bytes are 0xFF, false otherwise
 */
static bool flash_verify_erased(ULONG address, ULONG size)
{
  bool failed = false;
  UBYTE d;

  printf("Verifying erase from 0x%08lX (%lu bytes)...\n", (unsigned long)address, (unsigned long)size);

  for (ULONG i = address; i < address + size; i++) {
    d = flash_readByte(i);
    if (d != 0xFF) {
      failed = true;
      printf("Erase verify failed at 0x%08lX: expected 0xFF, got 0x%02X\n", (unsigned long)i, d);
    }
  }

  if (failed) {
    printf("ERASE VERIFICATION FAILED for region 0x%08lX-0x%08lX\n",
           (unsigned long)address, (unsigned long)(address + size - 1));
  } else {
    printf("Erase verification successful!\n");
  }

  return !failed;
}

/**
 * flash_erase_chip
 *
 * @brief Erase entire flash chip (dispatches to SPI or parallel)
 * @return true if erase and verification successful, false otherwise
 */
bool flash_erase_chip(void)
{
  bool result = false;

#ifdef FLASH_SPI
  if (current_flash_type == FLASH_TYPE_SPI) {
    result = spi_flash_erase_chip();
  }
#endif
#ifdef FLASH_PARALLEL
  if (current_flash_type == FLASH_TYPE_PARALLEL) {
    result = parallel_flash_erase_chip();
  }
#endif

  // Only verify if erase succeeded
  return result && flash_verify_erased(0, flash_size);
}

/**
 * flash_erase_sector
 *
 * @brief Erase a flash sector (dispatches to SPI or parallel) and verify
 * @param address Starting address
 * @param sectorSize Size of sector to erase
 * @return true if erase and verification successful, false otherwise
 */
bool flash_erase_sector(ULONG address, ULONG sectorSize)
{
  bool result = false;

#ifdef FLASH_SPI
  if (current_flash_type == FLASH_TYPE_SPI) {
    result = spi_flash_erase_sector(address, sectorSize);
  }
#endif
#ifdef FLASH_PARALLEL
  if (current_flash_type == FLASH_TYPE_PARALLEL) {
    parallel_flash_erase_sector(address, sectorSize);
    result = true;
  }
#endif

  // Only verify if erase succeeded
  return result && flash_verify_erased(address, sectorSize);
}

/**
 * flash_erase_bank
 *
 * @brief Erase a flash bank (dispatches to SPI or parallel) and verify
 * @param address Starting address of the bank
 * @param sectorSize Sector size
 * @param bankSize Bank size
 * @return true if erase and verification successful, false otherwise
 */
bool flash_erase_bank(ULONG address, ULONG sectorSize, ULONG bankSize)
{
  bool result = false;

#ifdef FLASH_SPI
  (void)sectorSize;
  if (current_flash_type == FLASH_TYPE_SPI) {
    // For SPI, erase starting at address for bankSize
    result = spi_flash_erase_sector(address, bankSize);
  }
#endif
#ifdef FLASH_PARALLEL
  if (current_flash_type == FLASH_TYPE_PARALLEL) {
    parallel_flash_erase_bank(address, sectorSize, bankSize);
    result = true;
  }
#endif

  // Only verify if erase succeeded
  return result && flash_verify_erased(address, bankSize);
}

/**
 * flash_cleanup
 *
 * @brief Cleanup flash operations (restore CPU state if needed)
 */
void flash_cleanup(void)
{
#ifdef FLASH_SPI
  if (current_flash_type == FLASH_TYPE_SPI) {
    spi_flash_cleanup();
  }
#endif
  // Parallel flash doesn't need cleanup
}
