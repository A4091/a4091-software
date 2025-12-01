/* spiutil.c — AmigaOS CLI SPI flash helper for Zorro-III SPI bridge
 *
 * Bridge protocol description:
 *  - Write BASE+0x7FFFF8 : assert /CS, shift out 1 byte, keep /CS active
 *  - Write BASE+0x7FFFFC : assert /CS, shift out 1 byte, then deassert /CS
 *  - Read  BASE+0x7FFFF8 : assert /CS, shift in 1 byte,  keep /CS active
 *  - Read  BASE+0x7FFFFC : assert /CS, shift in 1 byte,  then deassert /CS
 *
 * SPI: mode 0, ~25 MHz. Commands: 0x9F, 0x06, 0x05, 0x02, 0x03, 0xD8.
 *
 * A4092 requires 32-bit MMIO with nibble-lane mapping:
 * - For writes: high nibble -> bits 28..31, low nibble -> bits 12..15
 * - For reads:  same extraction from a 32-bit value
 */

/* AmigaOS includes for auto-detect via expansion.library */
#include <exec/execbase.h>
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <libraries/expansion.h>

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <proto/dos.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>


#include <dos/dos.h>
#include <proto/alib.h>
#include <dos/dosextens.h>

/* Use mirrored addresses for writes to work around 68030 write-allocation bug.
 * The MMU must be configured to make the write-mirror non-cacheable.
 * For now however, we disable write-allocation instead of using different
 * addresses.
 */
#define SPI_PORT_READ_HOLD_OFFS  0x7FFFF8
#define SPI_PORT_READ_END_OFFS   0x7FFFFC
#define SPI_PORT_WRITE_HOLD_OFFS 0x7FFFF8 // 0x7FFFF0
#define SPI_PORT_WRITE_END_OFFS  0x7FFFFC // 0x7FFFF4

/* A4092 board identification (matches main.c) */
#define MANUF_ID_COMMODORE_BRAUNSCHWEIG 513
#define MANUF_ID_COMMODORE             514
#define PROD_ID_A4092                   84

#if __GNUC__ < 11
struct ExecBase *SysBase;
#endif
struct ExpansionBase *ExpansionBase = NULL;

#undef FindConfigDev
// NDK 1.3 definition of FindConfigDev is incorrect which causes "makes pointer from integer without a cast" warning
struct ConfigDev* FindConfigDev(struct ConfigDev*, LONG, LONG);

#include "cpu_support.h"

#define MAX_TRANSFER_SIZE (8 * 1024 * 1024)

static inline uint32_t pack_nibble_word(uint8_t b)
{
    uint32_t hi = ((uint32_t)((b >> 4) & 0x0F)) << 28; /* bits 28..31 */
    uint32_t lo = ((uint32_t)( b       & 0x0F)) << 12; /* bits 12..15 */
    return hi | lo;
}
static inline uint8_t unpack_nibble_word(uint32_t w)
{
    uint8_t hi = (uint8_t)((w >> 28) & 0x0F);
    uint8_t lo = (uint8_t)((w >> 12) & 0x0F);
    return (uint8_t)((hi << 4) | lo);
}
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
    SPI_CMD_WREN   = 0x06,
    SPI_CMD_RDSR1  = 0x05,
    SPI_CMD_WRSR   = 0x01,
    SPI_CMD_RDSR2  = 0x35,
    SPI_CMD_RDID   = 0x9F,
    SPI_CMD_READ   = 0x03,
    SPI_CMD_PP     = 0x02,
    SPI_CMD_BE64K  = 0xD8,
};

#define SR1_WIP     0x01
#define SR1_WEL     0x02
#define SR1_BP_MASK 0x1C

#define SR2_QE      0x02

#define SPI_PAGE_SIZE   256u
#define SPI_BLOCK_SIZE  65536u

/* ===== low-level bytewise helpers ===== */
static inline void spi_tx_hold(uint32_t base, uint8_t v) { mmio_write_hold(base, v); }
static inline void spi_tx_end (uint32_t base, uint8_t v) {
    mmio_write_end (base, v);
    //for (volatile int i = 0; i < 200; i++);
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
    const uint32_t settle_polls = 4096;
    bool saw_wip = false;

    /* Allow the SPI engine to finish clocking the opcode before polling aggressively. */
    for (uint32_t i = 0; i < settle_polls; ++i) {
        uint8_t sr = spi_read_sr1(base);
        if (sr & SR1_WIP) {
            saw_wip = true;
            break;
        }
    }

    if (!saw_wip) {
        fprintf(stderr, "SPI: command never asserted WIP (SR1=%02X)\n", spi_read_sr1(base));
        return false;
    }

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

static bool spi_clear_block_protect(uint32_t base)
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
static void spi_read_id(uint32_t base, uint8_t *mfg, uint8_t *type, uint8_t *cap)
{
    spi_tx_hold(base, SPI_CMD_RDID);
    if (mfg)  *mfg  = spi_rx_hold(base);
    if (type) *type = spi_rx_hold(base);
    if (cap)  *cap  = spi_rx_end(base);
}
static bool spi_block_erase(uint32_t base, uint32_t baddr)
{
    spi_write_enable(base);
    if (!spi_wait_wel_set(base, 1000)) {
        fprintf(stderr, "ERROR: WEL not set before erase at 0x%08lX\n", (unsigned long)baddr);
        return false;
    }
    spi_tx_hold(base, SPI_CMD_BE64K);
    spi_tx_hold(base, (baddr >> 16) & 0xFF);
    spi_tx_hold(base, (baddr >> 8)  & 0xFF);
    spi_tx_end (base,  baddr        & 0xFF);
    return spi_wait_wip_clear(base, 2000000);
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
static bool spi_read_buf(uint32_t base, uint32_t addr, uint8_t *out, size_t len)
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
static bool spi_write_buf_pagewise(uint32_t base, uint32_t addr, const uint8_t *in, size_t len)
{
    while (len) {
        uint32_t page_off  = addr & (SPI_PAGE_SIZE-1);
        uint32_t page_room = SPI_PAGE_SIZE - page_off;
        size_t chunk = len < page_room ? len : page_room;
        if (!spi_page_program(base, addr, in, chunk)) return false;
        addr += chunk; in += chunk; len -= chunk;
    }
    return true;
}
static bool spi_erase_range_blocks(uint32_t base, uint32_t addr, size_t len,
                                   void (*progress)(size_t done, size_t total))
{
    uint32_t start = (addr / SPI_BLOCK_SIZE) * SPI_BLOCK_SIZE;
    uint32_t end   = ((addr + (uint32_t)len - 1) / SPI_BLOCK_SIZE) * SPI_BLOCK_SIZE;
    size_t total = (end - start) / SPI_BLOCK_SIZE + 1, k = 0;
    for (uint32_t b = start; b <= end; b += SPI_BLOCK_SIZE, ++k) {
        if (progress) progress(k, total);
        if (!spi_block_erase(base, b)) return false;
    }
    if (progress) progress(total, total);
    return true;
}
static bool spi_verify_buf(uint32_t base, uint32_t addr, const uint8_t *ref, size_t len,
                           void (*progress)(size_t done, size_t total))
{
    const size_t CH = 4096;
    uint8_t *tmp = (uint8_t*)malloc(CH);
    if (!tmp) return false;
    size_t done = 0;
    while (len) {
        size_t n = len > CH ? CH : len;
        if (!spi_read_buf(base, addr, tmp, n)) { free(tmp); return false; }
        for (size_t i=0; i<n; ++i) {
            if (tmp[i] != ref[i]) { free(tmp); return false; }
        }
        addr += n; ref += n; len -= n; done += n;
        if (progress) progress(done, done + len);
    }
    free(tmp);
    if (progress) progress(1,1);
    return true;
}

static bool spi_verify_erased(uint32_t base, uint32_t addr, size_t len,
                              void (*progress)(size_t done, size_t total))
{
    const size_t CH = 4096;
    uint8_t *tmp = (uint8_t*)malloc(CH);
    if (!tmp) return false;
    size_t done = 0;
    while (len) {
        size_t n = len > CH ? CH : len;
        if (!spi_read_buf(base, addr, tmp, n)) { free(tmp); return false; }
        for (size_t i=0; i<n; ++i) {
            if (tmp[i] != 0xFF) {
                printf("Verification failed at address 0x%08lX, expected 0xFF, got 0x%02X\n", (unsigned long)(addr + i), tmp[i]);
                free(tmp);
                return false;
            }
        }
        addr += n; len -= n; done += n;
        if (progress) progress(done, done + len);
    }
    free(tmp);
    if (progress) progress(1,1);
    return true;
}

/* ===== util ===== */
static void bar_progress(size_t done, size_t total, const char *label) {
    const int W = 46;
    int fill = 0;
    int pct = 100;
    if (total > 0) {
        /* integer math with rounding */
        fill = (int)((done * (size_t)W + total / 2) / total);
        pct  = (int)((done * (size_t)100 + total / 2) / total);
        if (fill > W) fill = W;
        if (pct > 100) pct = 100;
    }
    printf("\r%-6s [", label);
    for (int i=0;i<W;i++) putchar(i<fill ? '#' : ' ');
    printf("] %3d%%", pct);
    fflush(stdout);
    if (total > 0 && done >= total) printf("\n");
}
static void bar_cb(size_t done, size_t total) { bar_progress(done, total, "erase"); }
static void bar_cb_verify(size_t done, size_t total) { bar_progress(done, total, "verify"); }
static void bar_cb_erase_verify(size_t done, size_t total) { bar_progress(done, total, "verify"); }

static bool read_file(const char *path, uint8_t **outp, size_t *outlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open '%s': %s\n", path, strerror(errno)); return false; }
    fseek(f, 0, SEEK_END);
    long L = ftell(f);
    if (L < 0) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t*)malloc((size_t)L);
    if (!buf) { fclose(f); return false; }
    if ((long)fread(buf,1,(size_t)L,f) != L) { fclose(f); free(buf); return false; }
    fclose(f);
    *outp = buf; *outlen = (size_t)L;
    return true;
}
static bool write_file(const char *path, const uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "open '%s' for write: %s\n", path, strerror(errno)); return false; }
    bool ok = fwrite(buf,1,len,f) == len;
    fclose(f);
    return ok;
}
static bool parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end==s || *end!='\0' || v>0xFFFFFFFFul) return false;
    *out = (uint32_t)v; return true;
}
static bool parse_size(const char *s, size_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end==s) return false;
    if (*end=='K' || *end=='k') {
        if (v > ULONG_MAX / 1024ul) return false;
        v *= 1024ul; ++end;
    }
    else if (*end=='M' || *end=='m') {
        if (v > ULONG_MAX / (1024ul*1024ul)) return false;
        v *= 1024ul*1024ul; ++end;
    }
    if (*end!='\0') return false;
    if (v > (unsigned long)SIZE_MAX) return false;
    *out = (size_t)v; return true;
}

/* ===== usage ===== */
static void usage(const char *argv0)
{
    printf(
"Usage:\n"
"  %s id\n"
"  %s read  <addr> <len> <outfile>\n"
"  %s erase <addr> <len> [--verify]\n"
"  %s write <addr> <infile> [--verify]\n"
"  %s verify <addr> <infile>\n"
"  %s patch <addr> <hexbytes>\n"
"  %s status\n"
"\n"
"Notes:\n"
"  - addr/len accept decimal or 0xHEX; len also supports K/M suffix (e.g. 64K).\n"
"  - 'write' assumes the destination is erased; run 'erase' first.\n"
"  - 'patch' writes a few bytes (e.g. 0xDE,0xAD,0xBE,0xEF) within an erased area.\n"
, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

/* parse CSV of hex bytes for 'patch' */
static bool parse_hexbytes(const char *s, uint8_t **out, size_t *outlen)
{
    const size_t max_hex_chars = MAX_TRANSFER_SIZE * 6; /* avoid unbounded input */
    size_t slen = strlen(s);
    if (slen == 0 || slen > max_hex_chars) return false;
    size_t capacity = slen + 1;
    uint8_t *buf = (uint8_t*)malloc(capacity);
    if (!buf) return false;
    size_t n=0;
    const char *p=s;
    while (*p) {
        while (*p==' '||*p=='\t'||*p==',') ++p;
        if (!*p) break;
        unsigned v;
        if (strncmp(p,"0x",2)==0 || strncmp(p,"0X",2)==0) {
            p+=2;
        }
        int nibbles=0; v=0;
        while ((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')||(*p>='A'&&*p<='F')) {
            v = (v<<4) | (unsigned)((*p>='0'&&*p<='9')?(*p-'0'):((*p>='a'&&*p<='f')?(*p-'a'+10):(*p-'A'+10)));
            ++p; ++nibbles;
            if (nibbles>2) break;
        }
        if (nibbles==0) { free(buf); return false; }
        if (n >= MAX_TRANSFER_SIZE || n >= capacity) { free(buf); return false; }
        buf[n++] = (uint8_t)v;
        while (*p==' '||*p=='\t'||*p==',') ++p;
        if (*p=='\0') break;
    }
    *out = buf; *outlen = n; return true;
}

/* ===== main command handlers ===== */
static int cmd_id(uint32_t base)
{
    uint8_t mfg=0, type=0, cap=0;
    spi_read_id(base, &mfg, &type, &cap);
    printf("JEDEC ID: %02X %02X %02X\n", mfg, type, cap);
    switch (mfg) {
    case 0xEF:
	    printf("Winbond ");
	    break;
    default:
	    printf("Unknown ");
	    break;
    }
    switch (type << 8 | cap) {
    case 0x4017:
	    printf("W25Q64");
	    break;
    default:
	    printf("Unknown ");
	    break;
    }
    printf("\n");
    return 0;
}

static int cmd_read(uint32_t base, uint32_t addr, size_t len, const char *outfile)
{
    if (len > MAX_TRANSFER_SIZE) { fprintf(stderr,"len too large (max %d)\n", MAX_TRANSFER_SIZE); return 2; }
    uint8_t *buf = (uint8_t*)malloc(len);
    if (!buf) { fprintf(stderr,"OOM\n"); return 2; }
    printf("Reading 0x%zx bytes from 0x%08lX...\n", len, (unsigned long)addr);
    if (!spi_read_buf(base, addr, buf, len)) { free(buf); fprintf(stderr,"SPI read failed\n"); return 3; }
    if (!write_file(outfile, buf, len)) { free(buf); fprintf(stderr,"write '%s' failed\n", outfile); return 3; }
    free(buf);
    printf("OK -> %s\n", outfile);
    return 0;
}
static int cmd_erase(uint32_t base, const char *saddr, const char *slen, bool do_verify)
{
    uint32_t addr; size_t len;
    if (!parse_u32(saddr,&addr) || !parse_size(slen,&len)) { fprintf(stderr,"bad addr/len\n"); return 2; }
    if (len > MAX_TRANSFER_SIZE) { fprintf(stderr,"len too large (max %d)\n", MAX_TRANSFER_SIZE); return 2; }
    if (!spi_clear_block_protect(base)) { return 3; }
    printf("Erasing range 0x%08lX .. 0x%08lX in 64KiB blocks\n",
           (unsigned long)addr, (unsigned long)(uint32_t)(addr+len-1));
    if (!spi_erase_range_blocks(base, addr, len, bar_cb)) { fprintf(stderr,"erase failed\n"); return 3; }
    printf("Erase OK\n");
    if (do_verify) {
        printf("Verifying erase...\n");
        bool ok = spi_verify_erased(base, addr, len, bar_cb_erase_verify);
        if (!ok) { fprintf(stderr,"verify FAILED\n"); return 4; }
        printf("Verify OK\n");
    }
    return 0;
}
static int cmd_write(uint32_t base, const char *saddr, const char *infile, bool do_verify)
{
    uint32_t addr; if (!parse_u32(saddr,&addr)) { fprintf(stderr,"bad addr\n"); return 2; }
    uint8_t *img=NULL; size_t len=0;
    if (!read_file(infile, &img, &len)) return 2;
    if (!spi_clear_block_protect(base)) { free(img); return 3; }
    printf("Programming %s (%zu bytes) to 0x%08lX (assumes erased)...\n", infile, len, (unsigned long)addr);
    /* page-wise program with a tiny progress print */
    size_t done=0;
    while (done<len) {
        size_t chunk = (len-done) > 4096 ? 4096 : (len-done);
        if (!spi_write_buf_pagewise(base, addr+done, img+done, chunk)) {
            fprintf(stderr,"program failed at +0x%zx\n", done);
            free(img); return 3;
        }
        done += chunk;
        bar_progress(done, len, "write");
    }
    if (do_verify) {
        printf("Verifying...\n");
        bool ok = spi_verify_buf(base, addr, img, len, bar_cb_verify);
        free(img);
        if (!ok) { fprintf(stderr,"verify FAILED\n"); return 4; }
        printf("Verify OK\n");
    } else {
        free(img);
    }
    printf("Write OK\n");
    return 0;
}
static int cmd_verify(uint32_t base, const char *saddr, const char *infile)
{
    uint32_t addr; if (!parse_u32(saddr,&addr)) { fprintf(stderr,"bad addr\n"); return 2; }
    uint8_t *img=NULL; size_t len=0;
    if (!read_file(infile, &img, &len)) return 2;
    printf("Verifying %s (%zu bytes) at 0x%08lX...\n", infile, len, (unsigned long)addr);
    bool ok = spi_verify_buf(base, addr, img, len, bar_cb_verify);
    free(img);
    if (!ok) { fprintf(stderr,"verify FAILED\n"); return 4; }
    printf("Verify OK\n");
    return 0;
}
static int cmd_patch(uint32_t base, const char *saddr, const char *hexlist)
{
    uint32_t addr; if (!parse_u32(saddr,&addr)) { fprintf(stderr,"bad addr\n"); return 2; }
    uint8_t *bytes=NULL; size_t n=0;
    if (!parse_hexbytes(hexlist, &bytes, &n) || n==0) { free(bytes); fprintf(stderr,"bad hex bytes\n"); return 2; }
    if (!spi_clear_block_protect(base)) { free(bytes); return 3; }
    printf("Patching %zu byte(s) at 0x%08lX (assumes erased)...\n", n, (unsigned long)addr);
    bool ok = spi_write_buf_pagewise(base, addr, bytes, n);
    free(bytes);
    if (!ok) { fprintf(stderr,"patch failed\n"); return 3; }
    printf("Patch OK\n"); return 0;
}

static int cmd_status(uint32_t base)
{
    uint8_t sr1 = spi_read_sr1(base);
    uint8_t sr2 = spi_read_sr2(base);
    printf("SR1: %02X (WIP=%u WEL=%u BP=%u)  SR2: %02X (QE=%u)\n",
           sr1,
           (sr1 & SR1_WIP) ? 1 : 0,
           (sr1 & SR1_WEL) ? 1 : 0,
           (sr1 >> 2) & 0x7,
           sr2,
           (sr2 & SR2_QE) ? 1 : 0);
    return 0;
}

/* ===== main ===== */
int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 1; }

    /* Locate A4092 board base via expansion.library */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
    SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop

    uint32_t base = 0;
    struct ConfigDev *cd = NULL;
    int boards_found = 0;
    if ((ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library",0)) != NULL) {
        while ((cd = FindConfigDev(cd,-1,-1)) != NULL) {
            switch (cd->cd_Rom.er_Manufacturer) {
                case MANUF_ID_COMMODORE:
                case MANUF_ID_COMMODORE_BRAUNSCHWEIG:
                    if (cd->cd_Rom.er_Product == PROD_ID_A4092) {
                        base = (uint32_t)(uintptr_t)cd->cd_BoardAddr;
                        boards_found++;
                        break;
                    } else {
                        continue;
                    }
                default:
                    continue;
            }
            if (base) break; /* Use first matching board */
        }
        CloseLibrary((struct Library *)ExpansionBase);
        ExpansionBase = NULL;
    }

    if (!base || boards_found == 0) {
        fprintf(stderr, "No A4092 board found via expansion.library\n");
        return 1;
    } else {
	printf("A4092 found at 0x%x\n", base);
    }

    int argi = 1;
    const char *cmd = argv[argi++];

    cpu_disable_write_allocation();
    atexit(cpu_restore_write_allocation);

    if (strcmp(cmd,"id")==0) {
        return cmd_id(base);
    }
    else if (strcmp(cmd,"read")==0) {
        if (argi+2 >= argc) { usage(argv[0]); return 1; }
        uint32_t addr; size_t len;
        if (!parse_u32(argv[argi], &addr) || !parse_size(argv[argi+1], &len)) { fprintf(stderr,"bad addr/len\n"); return 2; }
        if (len > MAX_TRANSFER_SIZE) { fprintf(stderr,"len too large (max %d)\n", MAX_TRANSFER_SIZE); return 2; }
        return cmd_read(base, addr, len, argv[argi+2]);
    }
    else if (strcmp(cmd,"erase")==0) {
        if (argi+1 >= argc) { usage(argv[0]); return 1; }
        bool verify = false;
        if (argi+2 < argc && strcmp(argv[argi+2],"--verify")==0) verify = true;
        return cmd_erase(base, argv[argi], argv[argi+1], verify);
    }
    else if (strcmp(cmd,"write")==0) {
        if (argi+1 >= argc) { usage(argv[0]); return 1; }
        bool verify = false;
        if (argi+2 < argc && strcmp(argv[argi+2],"--verify")==0) verify = true;
        return cmd_write(base, argv[argi], argv[argi+1], verify);
    }
    else if (strcmp(cmd,"verify")==0) {
        if (argi+1 >= argc) { usage(argv[0]); return 1; }
        return cmd_verify(base, argv[argi], argv[argi+1]);
    }
    else if (strcmp(cmd,"patch")==0) {
        if (argi+1 >= argc) { usage(argv[0]); return 1; }
        return cmd_patch(base, argv[argi], argv[argi+1]);
    }
    else if (strcmp(cmd,"status")==0) {
        return cmd_status(base);
    }

    usage(argv[0]);
    return 1;
}
