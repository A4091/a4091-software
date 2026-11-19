/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dosextens.h>
#include <exec/execbase.h>
#include "flash.h"

const char version[] = "$VER: bppcid 1.0 (18.10.2026)";
extern struct ExecBase *SysBase;
extern int __check_abort_enabled;
extern int bppc_mapping_ok(void);

#define REG8(addr) (*(volatile uint8_t *)(addr))
#define ROM_BASE 0x00f00000UL
#define RESET_REG 0x00f60000UL
#define LOCK_REG 0x00f60020UL
#define FLASH_UNLOCK 0x00f60092UL
#define FLASH_LOCK 0x00f60093UL
#define SCSI_RELEASED 0x01
#define PPC_RELEASED 0x10
#define CACHE_MASK (CACRF_EnableI | CACRF_EnableD | CACRF_EnableE | CACRF_CopyBack)

static uint8_t
rom_read(void *ctx, uint32_t offset)
{
    (void)ctx;
    return REG8(ROM_BASE + offset);
}

static void
rom_write(void *ctx, uint32_t offset, uint8_t value)
{
    (void)ctx;
    REG8(ROM_BASE + offset) = value;
    __asm__ volatile ("nop" : : : "memory");
}

/* Called with task switching forbidden; no force override for these guards. */
static int
software_busy(void)
{
    return FindName(&SysBase->DeviceList, "blizzppc.device") != NULL ||
           FindName(&SysBase->LibList, "ppc.library") != NULL ||
           FindName(&SysBase->LibList, "powerpc.library") != NULL ||
           FindName(&SysBase->ResourceList, "ppc.library") != NULL ||
           FindName(&SysBase->ResourceList, "powerpc.library") != NULL;
}

static int
check_board(void)
{
    ULONG cache;
    APTR stack;
    int busy, mapped;
    if (SysBase->LibNode.lib_Version < 37 ||
        !(SysBase->AttnFlags & AFF_68060) ||
        !FindResident("BlizzardPPC.IDTag")) {
        puts("Requires a Blizzard PPC with 68060 and Exec V37 or newer.");
        return 0;
    }
    Forbid();
    cache = CacheControl(0, CACHE_MASK);
    Disable();
    stack = SuperState();
    mapped = bppc_mapping_ok();
    if (stack)
        UserState(stack);
    Enable();
    CacheControl(cache, CACHE_MASK);
    Permit();
    if (!mapped) {
        puts("ROM/I/O is remapped or write-protected by the MMU. "
             "Boot without MMU protection/ROM remapping first.");
        return 0;
    }
    if ((REG8(LOCK_REG) & 0x0e) != 0x08) {
        puts("Board control register does not identify a Blizzard PPC.");
        return 0;
    }
    Forbid();
    busy = software_busy();
    Permit();
    if (busy) {
        puts("SCSI/PPC software is loaded. Boot without startup-sequence, "
             "holding S to disable SCSI.");
        return 0;
    }
    if (REG8(RESET_REG) & SCSI_RELEASED) {
        puts("SCSI reset must be asserted to expose the ROM. "
             "Boot holding S first.");
        return 0;
    }
    return 1;
}

static int
snapshot(uint8_t *image)
{
    uint32_t i;
    ULONG cache;
    int same = 1;
    Forbid();
    cache = CacheControl(0, CACHE_MASK);
    for (i = 0; i < BPPC_ROM_SIZE; i++)
        image[i] = rom_read(NULL, i);
    for (i = 0; i < BPPC_ROM_SIZE; i++)
        if (image[i] != rom_read(NULL, i)) {
            same = 0;
            break;
        }
    CacheControl(cache, CACHE_MASK);
    Permit();
    if (!same)
        puts("ROM changed between reads; refusing to continue.");
    return same;
}

/* The exclusive create is intentional: never replace an earlier backup. */
static int
save_backup(const char *path, const uint8_t *image)
{
    int fd, ok = 1;
    uint32_t offset = 0;
    uint8_t buffer[1024];
    ssize_t count;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        perror("Cannot create new backup");
        return 0;
    }
    while (offset < BPPC_ROM_SIZE) {
        count = write(fd, image + offset, BPPC_ROM_SIZE - offset);
        if (count <= 0) {
            ok = 0;
            break;
        }
        offset += count;
    }
    if (close(fd) != 0)
        ok = 0;
    if (!ok) {
        puts("Backup write failed. Flash has not been changed.");
        return 0;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("Cannot reopen backup");
        return 0;
    }
    offset = 0;
    while (offset < BPPC_ROM_SIZE) {
        count = read(fd, buffer, sizeof buffer);
        if (count <= 0 || (uint32_t)count > BPPC_ROM_SIZE - offset ||
            memcmp(buffer, image + offset, count)) {
            ok = 0;
            break;
        }
        offset += count;
    }
    if (ok && read(fd, buffer, 1) != 0)
        ok = 0;
    if (close(fd) != 0)
        ok = 0;
    if (!ok) {
        puts("Backup readback failed. Flash has not been changed.");
        return 0;
    }
    printf("Verified backup: %s (%lu bytes, CRC32 %08lx)\n", path,
           (unsigned long)BPPC_ROM_SIZE,
           (unsigned long)bppc_crc32(image, BPPC_ROM_SIZE));
    return 1;
}

static int
backup_is_persistent(const char *path)
{
    struct DevProc *dest = GetDeviceProc((STRPTR)path, NULL);
    struct DevProc *ram = GetDeviceProc((STRPTR)"RAM:", NULL);
    int ok = dest && ram && dest->dvp_Port != ram->dvp_Port;
    if (dest)
        FreeDeviceProc(dest);
    if (ram)
        FreeDeviceProc(ram);
    if (!ok)
        puts("Writing requires a backup on an accessible non-RAM: device.");
    return ok;
}

static int
update(const uint8_t *original, const uint8_t *sector)
{
    struct flash_io io = { NULL, rom_read, rom_write, 100000000UL, 1000000UL };
    struct flash_result result;
    uint8_t reset;
    ULONG cache;
    int stopped, restored;
    APTR stack;
    static const char * const errors[] = {
        "success", "unsupported flash ID (requires AMD 01:a4)",
        "boot sector is protected", "ROM changed since backup",
        "sector erase failed", "byte programming failed", "verification failed",
        "invalid ROM/sector edit plan"
    };

    puts("Rewriting boot sector. Do not reset or remove power.");
    fflush(NULL);
    Forbid();
    if (software_busy() || (REG8(RESET_REG) & SCSI_RELEASED)) {
        Permit();
        puts("Board/software state changed; flash was not touched.");
        return 10;
    }
    cache = CacheControl(0, CACHE_MASK);
    Disable();
    stack = SuperState();
    restored = bppc_mapping_ok();
    if (stack)
        UserState(stack);
    if (!restored) {
        Enable();
        CacheControl(cache, CACHE_MASK);
        Permit();
        puts("ROM/I/O mapping changed; flash was not touched.");
        return 10;
    }
    reset = REG8(RESET_REG);
    REG8(RESET_REG) = PPC_RELEASED; /* Clear only PPC release. */
    __asm__ volatile ("nop" : : : "memory");
    stopped = !(REG8(RESET_REG) & PPC_RELEASED);
    if (stopped) {
        REG8(FLASH_UNLOCK) = 0x42;
        __asm__ volatile ("nop" : : : "memory");
        bppc_flash_update(&io, original, sector, &result);
        REG8(FLASH_LOCK) = 0x42;
        __asm__ volatile ("nop" : : : "memory");
    }
    /* Do not restart the PPC into an unverified boot sector. */
    if ((!stopped || !result.destructive || result.status == FLASH_OK ||
         result.recovered) && (reset & PPC_RELEASED)) {
        REG8(RESET_REG) = 0x80 | PPC_RELEASED;
        __asm__ volatile ("nop" : : : "memory");
    }
    restored = !((REG8(RESET_REG) ^ reset) & (PPC_RELEASED | SCSI_RELEASED));
    Enable();
    CacheControl(cache, CACHE_MASK);
    Permit();
    if (!stopped) {
        puts("Could not hold PPC in reset; flash was not touched.");
        return 10;
    }
    printf("Flash JEDEC %02x:%02x, sector protection %02x\n",
           result.manufacturer, result.device, result.protection);
    printf("CPU/SCSI reset state: %s\n", restored ? "restored" : "NOT restored");
    if (result.status != FLASH_OK) {
        printf("Update failed: %s (offset %08lx).\n", errors[result.status],
               (unsigned long)result.offset);
        if (!result.destructive)
            puts("No erase or programming was attempted.");
        else if (result.recovered)
            puts("Original boot sector restored and all mapped ROM verified.");
        else
            puts("RECOVERY FAILED: boot sector is not verified. PPC remains "
                 "in reset. Keep the backup for external-programmer recovery.");
        return 20;
    }
    puts("Verified: only ROM offset 00000010 changed. "
         "All other mapped ROM bytes match the backup.");
    puts("Power OFF completely, then boot and run ncr7xx -a bppc -R.");
    return restored ? 0 : 20;
}

static void
usage(void)
{
    puts("bppcid - Blizzard PPC 060 ROM SCSI identity editor\n"
         "  bppcid\n"
         "  bppcid --backup file\n"
         "  bppcid --set-id IAH0029 --expect-id HAH0029 [--backup file] [--write]\n"
         "Only the leading H/I may change; all other ID bytes must match.\n"
         "Without --write, no flash commands are issued.\n"
         "--write requires --backup on persistent storage (not RAM:).\n"
         "Backup covers mapped ROM f00000-f5ffff (384 KiB), not all 512 KiB.\n"
         "Boot without startup-sequence; hold S to disable SCSI.");
}

int
main(int argc, char **argv)
{
    const char *target = NULL, *expected = NULL, *backup = NULL;
    uint8_t *original = NULL, *sector = NULL;
    int write_flash = 0, i, rc = 10;
    uint32_t sum;
    __check_abort_enabled = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else if (!strcmp(argv[i], "--write") && !write_flash) {
            write_flash = 1;
        } else if (!strcmp(argv[i], "--set-id") && !target && i + 1 < argc) {
            target = argv[++i];
        } else if (!strcmp(argv[i], "--expect-id") && !expected && i + 1 < argc) {
            expected = argv[++i];
        } else if (!strcmp(argv[i], "--backup") && !backup && i + 1 < argc) {
            backup = argv[++i];
        } else {
            usage();
            return 10;
        }
    }
    if ((target && (!expected || strlen(target) != 7 || strlen(expected) != 7)) ||
        (!target && expected) || (write_flash && (!target || !backup))) {
        usage();
        return 10;
    }
    if (write_flash && !backup_is_persistent(backup))
        return 10;
    if (!check_board())
        return 10;
    original = malloc(BPPC_ROM_SIZE);
    sector = malloc(BPPC_SECTOR_SIZE);
    if (!original || !sector) {
        puts("Not enough memory for ROM backup and sector buffer.");
        goto out;
    }
    if (!snapshot(original))
        goto out;
    if (original[0] != 0x11 || original[1] != 0x11 ||
        !bppc_valid_id(original + BPPC_ID_OFFSET)) {
        puts("Unrecognized ROM header/060 identity; refusing to continue.");
        goto out;
    }
    printf("Current ID: %.7s\n", original + BPPC_ID_OFFSET);
    sum = bppc_rom_sum(original);
    printf("Bootstrap checksum: %08lx (%s)\n", (unsigned long)sum,
           sum ? "INVALID" : "valid");
    if (sum) {
        puts("Unrecognized/corrupt firmware; flash writes are disabled.");
        /* A read-only backup is still useful for diagnosis. */
        if (backup && !target)
            rc = save_backup(backup, original) ? 0 : 10;
        goto out;
    }
    if (target) {
        if (!bppc_plan_id(original, expected, target)) {
            puts("ID mismatch or invalid change. Only H <-> I is permitted.");
            goto out;
        }
        printf("Proposed ID: %s -> %s; only offset 00000010 (%02x -> %02x).\n",
               expected, target, (unsigned char)expected[0],
               (unsigned char)target[0]);
        memcpy(sector, original, BPPC_SECTOR_SIZE);
        sector[BPPC_ID_OFFSET] = target[0];
    }
    if (backup && !save_backup(backup, original))
        goto out;
    if (write_flash)
        rc = update(original, sector);
    else {
        puts("Read-only operation complete; flash was not changed.");
        rc = 0;
    }
out:
    free(sector);
    free(original);
    return rc;
}
