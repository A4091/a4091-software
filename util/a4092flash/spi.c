#include <exec/execbase.h>
#include <proto/exec.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "spi.h"
#include "nibble_word.h"

// This is a stand-in until I get SysBase properly
extern struct ExecBase *SysBase;

/* Global variable to store the original CACR state for 68030 */
ULONG g_OriginalCACR = 0;

/* Use mirrored addresses for writes to work around 68030 write-allocation bug.
 * The MMU must be configured to make the write-mirror non-cacheable.
 * For now however, we disable write-allocation instead of using different
 * addresses.
 */
#define SPI_PORT_READ_HOLD_OFFS  0x7FFFF8
#define SPI_PORT_READ_END_OFFS   0x7FFFFC
#define SPI_PORT_WRITE_HOLD_OFFS 0x7FFFF8 // 0x7FFFF0
#define SPI_PORT_WRITE_END_OFFS  0x7FFFFC // 0x7FFFF4

/* Forward declarations for supervisor functions */
ULONG __stdargs Sup_DisableWA(void);
ULONG __stdargs Sup_RestoreWA(void);

void spi_disable_burst(void) {
    /* 1. Check if we are actually on a 68030 */
    if (!(SysBase->AttnFlags & AFF_68030)) {
        return;
    }

    /* 2. Define supervisor function inline (must use RTE not RTS) and call it */
    __asm__ __volatile__ (
        "       bra     1f                     \n"
        "       .globl  _Sup_DisableWA         \n"
        "_Sup_DisableWA:                       \n"
        "       movec   %%cacr,%%d0            \n"  // Read CACR
        "       move.l  %%d0,_g_OriginalCACR   \n"  // Save it (note underscore prefix)
        "       btst    #8,%%d0                \n"  // Test DCE bit (bit 8)
        "       beq.s   .skip_disable          \n"  // Skip if cache not enabled
        "       bclr    #13,%%d0               \n"  // Clear WA bit (bit 13)
        "       movec   %%d0,%%cacr            \n"  // Write back
        ".skip_disable:                        \n"
        "       moveq   #0,%%d0                \n"  // Return 0
        "       rte                            \n"  // Return from exception
        "1:                                    \n"
        :
        :
        : "d0", "cc", "memory"
    );
    Supervisor(Sup_DisableWA);
}

void spi_restore_burst(void) {
    /* 1. Check if we are on a 68030 */
    if (!(SysBase->AttnFlags & AFF_68030)) {
        return;
    }

    /* 2. Define supervisor function inline (must use RTE not RTS) and call it */
    __asm__ __volatile__ (
        "       bra     1f                     \n"
        "       .globl  _Sup_RestoreWA         \n"
        "_Sup_RestoreWA:                       \n"
        "       move.l  _g_OriginalCACR,%%d0   \n"  // Load saved CACR (note underscore prefix)
        "       movec   %%d0,%%cacr            \n"  // Restore it
        "       moveq   #0,%%d0                \n"  // Return 0
        "       rte                            \n"  // Return from exception
        "1:                                    \n"
        :
        :
        : "d0", "cc", "memory"
    );
    Supervisor(Sup_RestoreWA);
}

/* MMIO access helpers using shared nibble_word functions */
static inline void mmio_write_hold(uint32_t base, uint8_t v)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(base + SPI_PORT_WRITE_HOLD_OFFS);
    *p = pack_nibble_word(v);
}
static inline void mmio_write_end(uint32_t base, uint8_t v)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(base + SPI_PORT_WRITE_END_OFFS);
    *p = pack_nibble_word(v);
}
static inline uint8_t mmio_read_hold(uint32_t base)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(base + SPI_PORT_READ_HOLD_OFFS);
    uint32_t w = *p;
    return unpack_nibble_word(w);
}
static inline uint8_t mmio_read_end(uint32_t base)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(base + SPI_PORT_READ_END_OFFS);
    uint32_t w = *p;
    return unpack_nibble_word(w);
}

/* ===== SPI commands / constants ===== */
enum {
    SPI_CMD_WREN    = 0x06,
    SPI_CMD_RDSR1   = 0x05,
    SPI_CMD_WRSR    = 0x01,
    SPI_CMD_RDSR2   = 0x35,
    SPI_CMD_RDID    = 0x9F,
    SPI_CMD_READ    = 0x03,
    SPI_CMD_PP      = 0x02,
    SPI_CMD_SE_4K   = 0x20, // New
    SPI_CMD_BE_32K  = 0x52, // New
    SPI_CMD_BE_64K  = 0xD8,
};

#define SR1_WIP     0x01
#define SR1_WEL     0x02
#define SR1_BP_MASK 0x1C

#define SR2_QE      0x02

/* ===== low-level bytewise helpers ===== */
static inline void spi_tx_hold(uint32_t base, uint8_t v) { mmio_write_hold(base, v); }
static inline void spi_tx_end (uint32_t base, uint8_t v) {
    mmio_write_end (base, v);
}
static inline uint8_t spi_rx_hold(uint32_t base) {
    return mmio_read_hold(base);
}
static inline uint8_t spi_rx_end (uint32_t base) {
    return mmio_read_end (base);
}

/* ===== status / waits ===== */
static void spi_write_enable(uint32_t base)
{
    spi_tx_end(base, SPI_CMD_WREN);
}
static uint8_t spi_read_sr1(uint32_t base)
{
    spi_tx_hold(base, SPI_CMD_RDSR1);
    return spi_rx_end(base);
}
static uint8_t spi_read_sr2(uint32_t base)
{
    spi_tx_hold(base, SPI_CMD_RDSR2);
    return spi_rx_end(base);
}
static bool spi_wait_wip_clear(uint32_t base, uint32_t max_iters)
{
    for (uint32_t i = 0; i < max_iters; ++i) {
        if ((spi_read_sr1(base) & SR1_WIP) == 0) {
            return true;
        }
    }
    fprintf(stderr, "SPI: timeout waiting for WIP clear (SR1=%02X)\n", spi_read_sr1(base));
    return false;
}
static bool spi_wait_wel_set(uint32_t base, uint32_t max_iters)
{
    while (max_iters--) {
        if (spi_read_sr1(base) & SR1_WEL) {
            return true;
        }
    }
    return false;
}

static bool spi_write_status(uint32_t base, uint8_t sr1, uint8_t sr2)
{
    spi_write_enable(base);
    if (!spi_wait_wel_set(base, 1000)) {
        fprintf(stderr, "ERROR: WEL not set before status write\n");
        return false;
    }
    spi_tx_hold(base, SPI_CMD_WRSR);
    spi_tx_hold(base, sr1);
    spi_tx_end(base, sr2);
    if (!spi_wait_wip_clear(base, 100000)) {
        fprintf(stderr, "ERROR: status register write timed out\n");
        return false;
    }
    return true;
}

bool spi_clear_block_protect(uint32_t base)
{
    uint8_t sr1 = spi_read_sr1(base);
    uint8_t sr2 = spi_read_sr2(base);
    if ((sr1 & SR1_BP_MASK) == 0) {
        return true;
    }
    uint8_t new_sr1 = sr1 & (uint8_t)~SR1_BP_MASK;
    printf("SPI flash reports block protection (SR1=%02X SR2=%02X); clearing BP bits...\n", sr1, sr2);
    if (!spi_write_status(base, new_sr1, sr2)) {
        fprintf(stderr, "ERROR: failed to clear block protection bits\n");
        return false;
    }
    uint8_t verify1 = spi_read_sr1(base);
    uint8_t verify2 = spi_read_sr2(base);
    if (verify1 & SR1_BP_MASK) {
        fprintf(stderr, "ERROR: block protection bits still set (SR1=%02X SR2=%02X). Check WP# pin.\n", verify1, verify2);
        return false;
    }
    return true;
}

/* ===== single ops ===== */
void spi_read_id(uint32_t base, uint8_t *mfg, uint8_t *type, uint8_t *cap)
{
    spi_tx_hold(base, SPI_CMD_RDID);
    if (mfg)  *mfg  = spi_rx_hold(base);
    if (type) *type = spi_rx_hold(base);
    if (cap)  *cap  = spi_rx_end(base);
}

static bool spi_block_erase(uint32_t base, uint32_t baddr, uint8_t erase_cmd)
{
    spi_write_enable(base);
    if (!spi_wait_wel_set(base, 1000)) {
        fprintf(stderr, "ERROR: WEL not set before erase at 0x%08lX\n", (unsigned long)baddr);
        return false;
    }
    spi_tx_hold(base, erase_cmd);
    spi_tx_hold(base, (baddr >> 16) & 0xFF);
    spi_tx_hold(base, (baddr >> 8)  & 0xFF);
    spi_tx_end (base,  baddr        & 0xFF);
    return spi_wait_wip_clear(base, 2000000);
}

bool spi_sector_erase_4k(uint32_t base, uint32_t baddr) {
    return spi_block_erase(base, baddr, SPI_CMD_SE_4K);
}

static bool spi_page_program(uint32_t base, uint32_t addr, const uint8_t *data, size_t len)
{
    if (!len || len > SPI_PAGE_SIZE) return false;
    spi_write_enable(base);
    if (!spi_wait_wel_set(base, 1000)) {
        fprintf(stderr, "ERROR: WEL not set before program at 0x%08lX\n", (unsigned long)addr);
        return false;
    }
    spi_tx_hold(base, SPI_CMD_PP);
    spi_tx_hold(base, (addr >> 16) & 0xFF);
    spi_tx_hold(base, (addr >> 8)  & 0xFF);
    spi_tx_hold(base,  addr        & 0xFF);
    for (size_t i=0; i+1<len; ++i) spi_tx_hold(base, data[i]);
    spi_tx_end(base, data[len-1]);
    return spi_wait_wip_clear(base, 100000);
}

/* ===== streaming helpers ===== */
bool spi_read_buf(uint32_t base, uint32_t addr, uint8_t *out, size_t len)
{
    if (!len) return true;
    spi_tx_hold(base, SPI_CMD_READ);
    spi_tx_hold(base, (addr >> 16) & 0xFF);
    spi_tx_hold(base, (addr >> 8)  & 0xFF);
    spi_tx_hold(base,  addr        & 0xFF);
    for (size_t i=0; i+1<len; ++i) out[i] = spi_rx_hold(base);
    out[len-1] = spi_rx_end(base);
    return true;
}
bool spi_write_buf_pagewise(uint32_t base, uint32_t addr, const uint8_t *in, size_t len, void (*progress)(size_t done, size_t total))
{
    size_t done = 0;
    size_t total = len;
    while (len) {
        uint32_t page_off  = addr & (SPI_PAGE_SIZE-1);
        uint32_t page_room = SPI_PAGE_SIZE - page_off;
        size_t chunk = len < page_room ? len : page_room;
        if (!spi_page_program(base, addr, in, chunk)) return false;
        addr += chunk; in += chunk; len -= chunk; done += chunk;
        if (progress) progress(done, total);
    }
    return true;
}

bool spi_erase_range_blocks(uint32_t base, uint32_t addr, size_t len)
{
    uint32_t start = (addr / SPI_BLOCK_SIZE) * SPI_BLOCK_SIZE;
    uint32_t end   = ((addr + (uint32_t)len - 1) / SPI_BLOCK_SIZE) * SPI_BLOCK_SIZE;
    for (uint32_t b = start; b <= end; b += SPI_BLOCK_SIZE) {
        if (!spi_block_erase(base, b, SPI_CMD_BE_64K)) return false;
    }
    return true;
}

void spi_erase_chip(uint32_t base, ULONG flashSize) {
    spi_erase_range_blocks(base, 0, flashSize);
}

void spi_erase_bank(uint32_t base, ULONG bankSize) {
    spi_erase_range_blocks(base, 0, bankSize);
}

/* ===== Abstraction layer implementation ===== */
/* These functions provide the interface expected by flash.c */

static uint32_t spi_base_address = 0;
static ULONG spi_flash_size = 0;

/**
 * spi_flash_init
 *
 * @brief Initialize and detect SPI flash
 * @param manuf Pointer to store manufacturer ID
 * @param devid Pointer to store device ID (capacity)
 * @param base Base address of the board
 * @param size Pointer to store flash size in bytes
 * @param sectorSize Pointer to store sector/block size (64KB for SPI)
 * @return true if SPI flash detected and initialized
 */
bool spi_flash_init(UBYTE *manuf, UBYTE *devid, volatile UBYTE *base, ULONG *size, ULONG *sectorSize)
{
    uint8_t mfg = 0, type = 0, cap = 0;
    spi_base_address = (uint32_t)(uintptr_t)base;

    // Enable SPI access (disable write allocation on 68030)
    spi_disable_burst();

    // Read JEDEC ID
    spi_read_id(spi_base_address, &mfg, &type, &cap);

    // Check if it's a valid SPI flash (manufacturer should not be 0xFF or 0x00)
    if (mfg == 0xFF || mfg == 0x00 || mfg == 0x9F) {
        spi_restore_burst();
        spi_base_address = 0;
        return false;
    }

    // Calculate flash size from capacity byte
    // Capacity byte is typically log2(size_in_bits) - 16
    // For W25Q64: cap=0x17 means 2^23 bits = 8MB
    ULONG flash_size_bits = 1UL << cap;
    spi_flash_size = flash_size_bits / 8;

    // Return values
    if (manuf) *manuf = mfg;
    if (devid) *devid = cap;  // Return capacity as device ID
    if (size) *size = spi_flash_size;
    if (sectorSize) *sectorSize = SPI_BLOCK_SIZE;  // 64KB blocks

    // Clear block protection
    spi_clear_block_protect(spi_base_address);

    // Print flash info
    const char *vendor = "Unknown";
    const char *device = "Unknown";

    switch (mfg) {
        case 0xEF:
            vendor = "Winbond";
            if (type == 0x40 && cap == 0x17) device = "W25Q64";
            else if (type == 0x40 && cap == 0x18) device = "W25Q128";
            break;
        case 0xC2:
            vendor = "Macronix";
            if (type == 0x20 && cap == 0x17) device = "MX25L6405";
            break;
    }

    printf("Flash part: %s %s (%lu KB)\n", vendor, device, (unsigned long)(spi_flash_size / 1024));

    return true;
}

/**
 * spi_flash_readByte
 *
 * @brief Read a single byte from SPI flash
 * @param address Byte address to read from
 * @return The byte read
 */
UBYTE spi_flash_readByte(ULONG address)
{
    uint8_t byte;
    spi_read_buf(spi_base_address, address, &byte, 1);
    return byte;
}

/**
 * spi_flash_writeByte
 *
 * @brief Write a single byte to SPI flash
 * @param address Byte address to write to
 * @param data Byte to write
 */
void spi_flash_writeByte(ULONG address, UBYTE data)
{
    spi_write_buf_pagewise(spi_base_address, address, &data, 1, NULL);
}

/**
 * spi_flash_erase_sector
 *
 * @brief Erase a sector (rounds up to 64KB block boundaries)
 * @param address Starting address
 * @param sectorSize Size of sector to erase
 */
void spi_flash_erase_sector(ULONG address, ULONG sectorSize)
{
    spi_erase_range_blocks(spi_base_address, address, sectorSize);
}

/**
 * spi_flash_erase_chip
 *
 * @brief Erase the entire SPI flash chip
 */
void spi_flash_erase_chip(void)
{
    spi_erase_range_blocks(spi_base_address, 0, spi_flash_size);
}

/**
 * spi_flash_cleanup
 *
 * @brief Cleanup SPI flash (restore CACR on 68030)
 */
void spi_flash_cleanup(void)
{
    spi_restore_burst();
}