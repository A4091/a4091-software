// SPDX-License-Identifier: GPL-2.0-only

#include <string.h>
#include <stdio.h>

#include <proto/exec.h>

#include "../../a4091.h"
#include "flash.h"
#include "hardware.h"

#define A4091_ROM_MAGIC1 0xFFFF5352
#define A4091_ROM_MAGIC2 0x2F434448
#define MAX_ROM_INSPECT_SIZE (64 * 1024)
#define VERSION_SEARCH_SIZE 4096

void setup_a4092_board(struct scsiBoard *board)
{
  board->flashbase = (volatile UBYTE *)board->cd->cd_BoardAddr;
}

const char *board_type_name(enum boardType type)
{
  switch (type) {
    case BOARD_A4091:
      return "A4091";
    case BOARD_A4092:
      return "A4092";
    case BOARD_A4770:
      return "A4770";
    default:
      return "None";
  }
}

const char *board_type_display_name(enum boardType type)
{
  switch (type) {
    case BOARD_A4091:
      return "A4091 SCSI-2 Host Controller";
    case BOARD_A4092:
      return "A4092 SCSI-2 Host Controller";
    case BOARD_A4770:
      return "A4770 Ultra-Wide SCSI Host Controller";
    default:
      return "No supported board detected";
  }
}

enum boardType board_type_from_configdev(const struct ConfigDev *cd)
{
  if (ZORRO_IS_A4092_ID(cd->cd_Rom.er_Manufacturer, cd->cd_Rom.er_Product))
    return BOARD_A4092;

  if (ZORRO_IS_A4770_ID(cd->cd_Rom.er_Manufacturer, cd->cd_Rom.er_Product))
    return BOARD_A4770;

  if (ZORRO_IS_LEGACY_A409X_ID(cd->cd_Rom.er_Manufacturer, cd->cd_Rom.er_Product))
    return BOARD_A4091;

  return BOARD_NONE;
}

static void copy_version_text(const UBYTE *buffer, ULONG size, ULONG offset,
                              char *version_out, ULONG version_max)
{
  size_t j = 0;

  if (!version_out || version_max == 0)
    return;

  while (j < (version_max - 1) && (offset + j) < size) {
    char c = buffer[offset + j];
    if (c == '\0' || c == '\n' || c == '\r')
      break;
    version_out[j++] = c;
  }

  version_out[j] = '\0';
}

static BOOL detect_version_in_buffer(UBYTE *buffer, ULONG size,
                                     enum boardType *type_out,
                                     char *version_out, ULONG version_max)
{
  static const struct {
    const char *needle;
    enum boardType type;
  } patterns[] = {
    { "A4770 scsidisk", BOARD_A4770 },
    { "A4092 scsidisk", BOARD_A4092 },
    { "A4091 scsidisk", BOARD_A4091 },
  };
  ULONG search_size = (size < VERSION_SEARCH_SIZE) ? size : VERSION_SEARCH_SIZE;

  for (ULONG p = 0; p < (sizeof(patterns) / sizeof(patterns[0])); p++) {
    size_t needle_len = strlen(patterns[p].needle);

    if (needle_len > search_size)
      continue;

    for (ULONG i = 0; i <= (search_size - needle_len); i++) {
      if (memcmp(buffer + i, patterns[p].needle, needle_len) == 0) {
        if (type_out)
          *type_out = patterns[p].type;
        copy_version_text(buffer, size, i, version_out, version_max);
        return TRUE;
      }
    }
  }

  return FALSE;
}

static BOOL detect_rom_size_from_buffer(UBYTE *buffer, ULONG size, ULONG *image_size)
{
  ULONG magic1, magic2;

  if (size >= 0x10000) {
    memcpy(&magic1, buffer + 0x10000 - 8, 4);
    memcpy(&magic2, buffer + 0x10000 - 4, 4);
    if (magic1 == A4091_ROM_MAGIC1 && magic2 == A4091_ROM_MAGIC2) {
      if (image_size)
        *image_size = 0x10000;
      return TRUE;
    }
  }

  if (size >= 0x8000) {
    memcpy(&magic1, buffer + 0x8000 - 8, 4);
    memcpy(&magic2, buffer + 0x8000 - 4, 4);
    if (magic1 == A4091_ROM_MAGIC1 && magic2 == A4091_ROM_MAGIC2) {
      if (image_size)
        *image_size = 0x8000;
      return TRUE;
    }
  }

  if (image_size)
    *image_size = 0;
  return FALSE;
}

static BOOL buffer_is_erased(UBYTE *buffer, ULONG size)
{
  for (ULONG i = 0; i < size; i++) {
    if (buffer[i] != 0xFF)
      return FALSE;
  }

  return TRUE;
}

void summarize_rom_buffer(UBYTE *buffer, ULONG size, struct romInfo *info)
{
  enum boardType version_type = BOARD_NONE;

  memset(info, 0, sizeof(*info));

  if (!buffer || size == 0) {
    strcpy(info->summary, "Empty");
    return;
  }

  info->erased = buffer_is_erased(buffer, size);
  if (info->erased) {
    strcpy(info->summary, "Flash is empty");
    return;
  }

  info->validImage = detect_rom_size_from_buffer(buffer, size, &info->imageSize);
  if (detect_version_in_buffer(buffer, size, &version_type, info->version,
                               sizeof(info->version))) {
    info->type = version_type;
    strncpy(info->summary, info->version, sizeof(info->summary) - 1);
    info->summary[sizeof(info->summary) - 1] = '\0';
    return;
  }

  if (info->validImage) {
    snprintf(info->summary, sizeof(info->summary), "Unknown %luKB ROM image",
             (unsigned long)(info->imageSize / 1024));
  } else {
    strcpy(info->summary, "Unrecognized data");
  }
}

BOOL inspect_flash_contents(ULONG flashSize, struct romInfo *info)
{
  ULONG inspectSize;
  UBYTE *buffer;
  BOOL ok = FALSE;

  if (!flashSize) {
    memset(info, 0, sizeof(*info));
    strcpy(info->summary, "No flash detected");
    return FALSE;
  }

  inspectSize = (flashSize < MAX_ROM_INSPECT_SIZE) ? flashSize : MAX_ROM_INSPECT_SIZE;
  buffer = AllocMem(inspectSize, MEMF_ANY);
  if (!buffer) {
    memset(info, 0, sizeof(*info));
    strcpy(info->summary, "Out of memory");
    return FALSE;
  }

  if (flash_readBuf(0, buffer, inspectSize)) {
    summarize_rom_buffer(buffer, inspectSize, info);
    ok = TRUE;
  } else {
    memset(info, 0, sizeof(*info));
    strcpy(info->summary, "Failed to read flash");
  }

  FreeMem(buffer, inspectSize);
  return ok;
}

BOOL read_mfg_data(struct mfg_data *mfg, BOOL *checksum_ok)
{
  const UBYTE *bytes;
  uint32_t xor = 0;

  if (!mfg)
    return FALSE;

  if (!flash_readBuf(MFG_FLASH_OFFSET, (UBYTE *)mfg, sizeof(*mfg)))
    return FALSE;

  if (mfg->magic != MFG_MAGIC)
    return FALSE;

  bytes = (const UBYTE *)mfg;
  for (ULONG i = 0; i < sizeof(*mfg); i += 4) {
    uint32_t word = ((uint32_t)bytes[i] << 24) |
                    ((uint32_t)bytes[i + 1] << 16) |
                    ((uint32_t)bytes[i + 2] << 8) |
                    ((uint32_t)bytes[i + 3]);
    xor ^= word;
  }

  if (checksum_ok)
    *checksum_ok = (xor == 0);
  return TRUE;
}
