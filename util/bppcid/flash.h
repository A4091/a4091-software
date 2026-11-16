/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BPPCID_FLASH_H
#define BPPCID_FLASH_H

#include <stdint.h>

#define BPPC_ROM_SIZE    0x60000UL
#define BPPC_SECTOR_SIZE 0x10000UL
#define BPPC_ID_OFFSET   0x10UL

struct flash_io {
    void *ctx;
    uint8_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint8_t);
    uint32_t erase_polls;
    uint32_t program_polls;
};

enum flash_status {
    FLASH_OK, FLASH_UNKNOWN, FLASH_PROTECTED, FLASH_CHANGED,
    FLASH_ERASE, FLASH_PROGRAM, FLASH_VERIFY, FLASH_PLAN
};

struct flash_result {
    enum flash_status status;
    uint32_t offset;
    uint8_t manufacturer, device, protection;
    int destructive;
    int recovered;
};

int bppc_valid_id(const uint8_t *id);
uint32_t bppc_rom_sum(const uint8_t *rom);
uint32_t bppc_crc32(const uint8_t *rom, uint32_t size);
int bppc_plan_id(const uint8_t *rom, const char *expected, const char *target);
void bppc_flash_update(const struct flash_io *io, const uint8_t *original,
                      const uint8_t *sector, struct flash_result *result);

#endif
