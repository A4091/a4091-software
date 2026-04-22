#ifndef HARDWARE_H
#define HARDWARE_H

#include <proto/expansion.h>

#include "main.h"
#include "mfg_flash.h"

enum boardType {
  BOARD_NONE = 0,
  BOARD_A4091,
  BOARD_A4092,
  BOARD_A4770
};

struct romInfo {
  enum boardType type;
  BOOL validImage;
  BOOL erased;
  ULONG imageSize;
  char version[128];
  char summary[160];
};

struct boardInfo {
  struct scsiBoard board;
  enum boardType type;
  BOOL present;
  BOOL flashAvailable;
  UBYTE manufId;
  UBYTE devId;
  ULONG flashSize;
  ULONG sectorSize;
};

void setup_a4092_board(struct scsiBoard *board);
const char *board_type_name(enum boardType type);
enum boardType board_type_from_configdev(const struct ConfigDev *cd);
void summarize_rom_buffer(UBYTE *buffer, ULONG size, struct romInfo *info);
BOOL inspect_flash_contents(ULONG flashSize, struct romInfo *info);
BOOL read_mfg_data(struct mfg_data *mfg, BOOL *checksum_ok);

#endif
