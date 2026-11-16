/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "flash.h"

struct model {
    uint8_t memory[0x80000];
    unsigned state, erases, programs, resets;
    uint8_t manufacturer, device, protection;
    int fail_erase, fail_program, timeout, fail_forever, corrupt_verify;
    uint32_t busy_offset;
    uint8_t busy_value;
};
static struct model model;
static uint8_t original[BPPC_ROM_SIZE], sector[BPPC_SECTOR_SIZE];

static uint8_t
read_byte(void *ctx, uint32_t offset)
{
    struct model *m = ctx;
    assert(offset < BPPC_ROM_SIZE);
    if (m->state == 9) {
        if (offset == 0) return m->manufacturer;
        if (offset == 1) return m->device;
        if (offset == 2) return m->protection;
        return 0xff;
    }
    if (m->state == 10 && offset == m->busy_offset)
        return ((m->busy_value ^ 0x80) & 0x80) | (m->timeout ? 0 : 0x20);
    if (m->corrupt_verify && m->erases == 1 && m->resets >= 2 &&
        offset == 0x20000) {
        m->corrupt_verify = 0;
        return m->memory[offset] ^ 1;
    }
    return m->memory[offset];
}

static void
write_byte(void *ctx, uint32_t offset, uint8_t value)
{
    struct model *m = ctx;
    assert(offset < BPPC_SECTOR_SIZE); /* No upper-sector command targets. */
    if (m->state == 7) {
        m->programs++;
        m->memory[offset] &= value; /* Real NOR cannot program a zero to one. */
        if (m->fail_program || m->fail_forever) {
            m->fail_program = 0;
            m->state = 10;
            m->busy_offset = offset;
            m->busy_value = value;
        } else {
            m->state = 0;
        }
        return;
    }
    if (value == 0xf0) {
        m->state = 0;
        m->resets++;
        return;
    }
    switch (m->state) {
    case 0:
        assert(offset == 0x5555 && value == 0xaa);
        m->state = 1;
        break;
    case 1:
        assert(offset == 0x2aaa && value == 0x55);
        m->state = 2;
        break;
    case 2:
        assert(offset == 0x5555);
        assert(value == 0x90 || value == 0x80 || value == 0xa0);
        m->state = value == 0x90 ? 9 : value == 0x80 ? 4 : 7;
        break;
    case 4:
        assert(offset == 0x5555 && value == 0xaa);
        m->state = 5;
        break;
    case 5:
        assert(offset == 0x2aaa && value == 0x55);
        m->state = 6;
        break;
    case 6:
        assert(offset == 0 && value == 0x30); /* Never chip erase (0x10). */
        m->erases++;
        if (m->fail_erase || m->fail_forever) {
            m->fail_erase = 0;
            m->busy_offset = 0;
            m->busy_value = 0xff;
            m->state = 10;
        } else {
            memset(m->memory, 0xff, BPPC_SECTOR_SIZE);
            m->state = 0;
        }
        break;
    default:
        assert(!"Unexpected flash command");
    }
}

static void
init(void)
{
    uint32_t i, sum;
    memset(&model, 0, sizeof model);
    for (i = 0; i < sizeof model.memory; i++)
        model.memory[i] = (uint8_t)((i * 29) ^ (i >> 8));
    memcpy(model.memory + BPPC_ID_OFFSET, "HAH0029", 8);
    memset(model.memory + 0x4fffc, 0, 4);
    sum = 0 - bppc_rom_sum(model.memory);
    for (i = 0; i < 4; i++)
        model.memory[0x4fffc + i] = (uint8_t)(sum >> (24 - i * 8));
    memcpy(original, model.memory, sizeof original);
    memcpy(sector, original, sizeof sector);
    sector[BPPC_ID_OFFSET] = 'I';
    model.manufacturer = 1;
    model.device = 0xa4;
}

static struct flash_result
run(void)
{
    struct flash_io io = { &model, read_byte, write_byte, 16, 16 };
    struct flash_result result;
    uint8_t tail[0x20000];
    memcpy(tail, model.memory + BPPC_ROM_SIZE, sizeof tail);
    bppc_flash_update(&io, original, sector, &result);
    assert(!memcmp(tail, model.memory + BPPC_ROM_SIZE, sizeof tail));
    assert(model.state == 0);
    return result;
}

int
main(void)
{
    struct flash_result result;
    uint32_t i;
    init();
    assert(bppc_valid_id(original + BPPC_ID_OFFSET));
    assert(!bppc_rom_sum(original));
    assert(bppc_plan_id(original, "HAH0029", "IAH0029"));
    assert(!bppc_plan_id(original, "HAH0028", "IAH0028"));
    assert(!bppc_plan_id(original, "HAH0029", "IDH0029"));
    assert(!bppc_plan_id(original, "HAH0029", "HAH0029"));
    assert(!bppc_plan_id(original, "HAH0029", "JAH0029"));
    assert(bppc_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926);
    result = run();
    assert(result.status == FLASH_OK && model.erases == 1);
    for (i = 0; i < BPPC_ROM_SIZE; i++)
        assert(model.memory[i] == (i == BPPC_ID_OFFSET ? 'I' : original[i]));
    assert(!bppc_rom_sum(model.memory));
    puts("H -> I, sector geometry, all other bytes and checksum: PASS");

    memcpy(original, model.memory, sizeof original);
    memcpy(sector, original, sizeof sector);
    sector[BPPC_ID_OFFSET] = 'H';
    assert(bppc_plan_id(original, "IAH0029", "HAH0029"));
    result = run();
    assert(result.status == FLASH_OK && model.memory[BPPC_ID_OFFSET] == 'H');
    puts("Reverse ID edit: PASS");

    init(); model.device = 0x20;
    result = run();
    assert(result.status == FLASH_UNKNOWN && !model.erases && !model.programs);
    init(); model.manufacturer = 0xbf;
    result = run();
    assert(result.status == FLASH_UNKNOWN && !model.erases);
    init(); model.protection = 1;
    result = run();
    assert(result.status == FLASH_PROTECTED && !model.erases);
    init(); model.memory[1234] ^= 1;
    result = run();
    assert(result.status == FLASH_CHANGED && result.offset == 1234 && !model.erases);
    puts("Unknown/protected flash and stale backup guards: PASS");

    init(); sector[1234] ^= 1;
    result = run();
    assert(result.status == FLASH_PLAN && !model.erases && !model.resets);
    init(); original[0x20000] ^= 1;
    result = run();
    assert(result.status == FLASH_PLAN && !model.erases && !model.resets);
    puts("Invalid sector edits and bad checksums rejected before flash commands: PASS");

    init(); model.fail_erase = 1;
    result = run();
    assert(result.status == FLASH_ERASE && result.recovered && model.erases == 2);
    assert(!memcmp(original, model.memory, sizeof original));
    init(); model.fail_program = 1;
    result = run();
    assert(result.status == FLASH_PROGRAM && result.recovered);
    assert(!memcmp(original, model.memory, sizeof original));
    init(); model.fail_program = 1; model.timeout = 1;
    result = run();
    assert(result.status == FLASH_PROGRAM && result.recovered);
    assert(!memcmp(original, model.memory, sizeof original));
    init(); model.corrupt_verify = 1;
    result = run();
    assert(result.status == FLASH_VERIFY && result.recovered);
    assert(!memcmp(original, model.memory, sizeof original));
    puts("Erase/program/timeout/verification failure restoration: PASS");

    init(); model.fail_forever = 1; model.timeout = 1;
    result = run();
    assert(result.status == FLASH_ERASE && !result.recovered && model.erases == 2);
    puts("Persistent failure is reported without unlimited retries: PASS");
    return 0;
}
