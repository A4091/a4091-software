// spi.h - Header for SPI flash functions

#ifndef SPI_H
#define SPI_H

#include <exec/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef FLASH_SPI

/* ===== CPU specific ===== */
void spi_disable_burst(void);
void spi_restore_burst(void);


/* ===== SPI commands / constants ===== */
#define SPI_PAGE_SIZE      256u
#define SPI_SECTOR_SIZE_4K 4096u
#define SPI_BLOCK_SIZE     65536u

/* Status register bits */
#define SR1_WIP     0x01
#define SR1_WEL     0x02
#define SR1_BP_MASK 0x1C

#define SR2_QE      0x02

/* ===== Core SPI functions ===== */
void spi_read_id(uint32_t base, uint8_t *mfg, uint8_t *type, uint8_t *cap);
void spi_read_unique_id(uint32_t base, uint8_t id[8]);
bool spi_clear_block_protect(uint32_t base);

/* ===== Status register access ===== */
uint8_t spi_read_sr1(uint32_t base);
uint8_t spi_read_sr2(uint32_t base);

/* ===== Streaming helpers ===== */
bool spi_read_buf(uint32_t base, uint32_t addr, uint8_t *out, size_t len);
bool spi_write_buf_pagewise(uint32_t base, uint32_t addr, const uint8_t *in, size_t len, void (*progress)(size_t done, size_t total));
bool spi_erase_range_blocks(uint32_t base, uint32_t addr, size_t len, void (*progress)(size_t done, size_t total));
bool spi_sector_erase_4k(uint32_t base, uint32_t baddr);
bool spi_erase_chip(uint32_t base, ULONG flashSize);
bool spi_erase_bank(uint32_t base, ULONG bankSize);

/* ===== Abstraction layer interface (called from flash.c) ===== */
bool spi_flash_init(UBYTE *manuf, UBYTE *devid, volatile UBYTE *base, ULONG *size, ULONG *sectorSize);
UBYTE spi_flash_readByte(ULONG address);
void spi_flash_writeByte(ULONG address, UBYTE data);
bool spi_flash_erase_sector(ULONG address, ULONG sectorSize);
bool spi_flash_erase_chip(void);
void spi_flash_cleanup(void);

#endif /* FLASH_SPI */

#endif // SPI_H
