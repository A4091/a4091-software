/* SPDX-License-Identifier: GPL-2.0-only */
#include "flash.h"

int
bppc_valid_id(const uint8_t *id)
{
    unsigned i;
    if ((id[0] != 'H' && id[0] != 'I') || id[1] < 'A' || id[1] > 'D' ||
        id[2] != 'H' || id[7] != 0)
        return 0;
    for (i = 3; i < 7; i++)
        if (!((id[i] >= '0' && id[i] <= '9') ||
              (id[i] >= 'A' && id[i] <= 'F')))
            return 0;
    return 1;
}

uint32_t
bppc_rom_sum(const uint8_t *rom)
{
    uint32_t sum = 0, i;
    for (i = 0x20; i < 0x50000; i += 4)
        sum += ((uint32_t)rom[i] << 24) | ((uint32_t)rom[i + 1] << 16) |
               ((uint32_t)rom[i + 2] << 8) | rom[i + 3];
    return sum;
}

uint32_t
bppc_crc32(const uint8_t *rom, uint32_t size)
{
    uint32_t crc = 0xffffffffUL, i;
    unsigned bit;
    for (i = 0; i < size; i++) {
        crc ^= rom[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320UL : 0);
    }
    return ~crc;
}

int
bppc_plan_id(const uint8_t *rom, const char *expected, const char *target)
{
    unsigned i;
    if (!bppc_valid_id(rom + BPPC_ID_OFFSET) ||
        !bppc_valid_id((const uint8_t *)expected) ||
        !bppc_valid_id((const uint8_t *)target))
        return 0;
    for (i = 0; i < 8; i++)
        if (rom[BPPC_ID_OFFSET + i] != (uint8_t)expected[i])
            return 0;
    for (i = 1; i < 8; i++)
        if (expected[i] != target[i])
            return 0;
    return expected[0] != target[0];
}

static void
command(const struct flash_io *io, uint8_t cmd)
{
    io->write(io->ctx, 0x5555, 0xaa);
    io->write(io->ctx, 0x2aaa, 0x55);
    io->write(io->ctx, 0x5555, cmd);
}

static int
poll(const struct flash_io *io, uint32_t offset, uint8_t wanted, uint32_t limit)
{
    while (limit--) {
        uint8_t value = io->read(io->ctx, offset);
        if (!((value ^ wanted) & 0x80))
            return 1;
        if (value & 0x20) {
            /* DQ7 may change at the same instant DQ5 becomes set. */
            value = io->read(io->ctx, offset);
            return !((value ^ wanted) & 0x80);
        }
    }
    return 0;
}

static enum flash_status
write_sector(const struct flash_io *io, const uint8_t *sector, uint32_t *bad)
{
    uint32_t i;
    command(io, 0x80);
    io->write(io->ctx, 0x5555, 0xaa);
    io->write(io->ctx, 0x2aaa, 0x55);
    io->write(io->ctx, 0, 0x30); /* Sector zero, never chip erase. */
    *bad = 0;
    if (!poll(io, 0, 0xff, io->erase_polls))
        return FLASH_ERASE;
    for (i = 0; i < BPPC_SECTOR_SIZE; i++) {
        if (io->read(io->ctx, i) != 0xff) {
            *bad = i;
            return FLASH_ERASE;
        }
    }
    for (i = 0; i < BPPC_SECTOR_SIZE; i++) {
        if (sector[i] == 0xff)
            continue;
        command(io, 0xa0);
        io->write(io->ctx, i, sector[i]);
        if (!poll(io, i, sector[i], io->program_polls) ||
            io->read(io->ctx, i) != sector[i]) {
            *bad = i;
            return FLASH_PROGRAM;
        }
    }
    return FLASH_OK;
}

static int
verify(const struct flash_io *io, const uint8_t *original,
       const uint8_t *sector, uint32_t *bad)
{
    uint32_t i;
    for (i = 0; i < BPPC_ROM_SIZE; i++) {
        uint8_t wanted = i < BPPC_SECTOR_SIZE ? sector[i] : original[i];
        if (io->read(io->ctx, i) != wanted) {
            *bad = i;
            return 0;
        }
    }
    return 1;
}

/* Caller keeps the PPC, interrupts and caches quiescent throughout. */
void
bppc_flash_update(const struct flash_io *io, const uint8_t *original,
                  const uint8_t *sector, struct flash_result *result)
{
    uint32_t rollback_bad, i;
    result->status = FLASH_OK;
    result->offset = 0;
    result->destructive = result->recovered = 0;
    result->manufacturer = result->device = result->protection = 0;
    if (bppc_rom_sum(original) ||
        !bppc_plan_id(original, (const char *)original + BPPC_ID_OFFSET,
                      (const char *)sector + BPPC_ID_OFFSET)) {
        result->status = FLASH_PLAN;
        return;
    }
    for (i = 0; i < BPPC_SECTOR_SIZE; i++) {
        if (i != BPPC_ID_OFFSET && sector[i] != original[i]) {
            result->status = FLASH_PLAN;
            result->offset = i;
            return;
        }
    }
    command(io, 0x90);
    result->manufacturer = io->read(io->ctx, 0);
    result->device = io->read(io->ctx, 1);
    result->protection = io->read(io->ctx, 2);
    io->write(io->ctx, 0, 0xf0);
    if (result->manufacturer != 0x01 || result->device != 0xa4) {
        result->status = FLASH_UNKNOWN;
        return;
    }
    if (result->protection != 0) {
        result->status = FLASH_PROTECTED;
        return;
    }
    if (!verify(io, original, original, &result->offset)) {
        result->status = FLASH_CHANGED;
        return;
    }
    result->destructive = 1;
    result->status = write_sector(io, sector, &result->offset);
    io->write(io->ctx, 0, 0xf0);
    if (result->status == FLASH_OK &&
        !verify(io, original, sector, &result->offset))
        result->status = FLASH_VERIFY;
    if (result->status != FLASH_OK) {
        /* One bounded restoration attempt, using the verified backup in RAM. */
        enum flash_status status = write_sector(io, original, &rollback_bad);
        io->write(io->ctx, 0, 0xf0);
        result->recovered = status == FLASH_OK &&
            verify(io, original, original, &rollback_bad);
    }
}
