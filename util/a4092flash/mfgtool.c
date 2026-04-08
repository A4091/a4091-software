/* spiutil.c — AmigaOS CLI SPI flash helper for Zorro-III SPI bridge
 *
 * Bridge protocol description:
 *  - Write BASE+0x7FFFC0 : assert /CS, shift out 1 byte, keep /CS active
 *  - Write BASE+0x7FFFC0 : assert /CS, shift out 1 byte, then deassert /CS
 *  - Read  BASE+0x7FFFE0 : assert /CS, shift in 1 byte,  keep /CS active
 *  - Read  BASE+0x7FFFF0 : assert /CS, shift in 1 byte,  then deassert /CS
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
#include <stddef.h>
#include <errno.h>
#include <limits.h>

#include "../../a4091.h"
#include "mfg_flash.h"
#include "oem_flash.h"


#include <dos/dos.h>
#include <proto/alib.h>
#include <dos/dosextens.h>
#include "../../ndkcompat.h"
#include "spi.h"

#if __GNUC__ < 11
struct ExecBase *SysBase;
#endif
struct ExpansionBase *ExpansionBase = NULL;

static bool board_has_legacy_a409x_id(const struct ConfigDev *cd)
{
    return ZORRO_IS_LEGACY_A409X_ID(cd->cd_Rom.er_Manufacturer,
                                    cd->cd_Rom.er_Product);
}

static bool board_has_a4092_id(const struct ConfigDev *cd)
{
    return ZORRO_IS_A4092_ID(cd->cd_Rom.er_Manufacturer,
                             cd->cd_Rom.er_Product);
}

static bool a4092_spi_flash_present(uint32_t base)
{
    uint8_t mfg = 0, type = 0, cap = 0;

    spi_read_id(base, &mfg, &type, &cap);
    if (mfg != 0xEF)
        return false;

    switch ((type << 8) | cap) {
        case 0x3013:
        case 0x4013:
        case 0x4017:
            return true;
        default:
            return false;
    }
}

#undef FindConfigDev
// NDK 1.3 definition of FindConfigDev is incorrect which causes "makes pointer from integer without a cast" warning
struct ConfigDev* FindConfigDev(struct ConfigDev*, LONG, LONG);

#define MAX_TRANSFER_SIZE (8 * 1024 * 1024)

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

    *out = (size_t)v; return true;
}

/* ===== mfg data helpers ===== */

static uint32_t mfg_checksum(const struct mfg_data *mfg)
{
    const uint32_t *words = (const uint32_t *)mfg;
    uint32_t xor = 0;
    /* XOR first 63 uint32_t values (252 bytes, everything before crc32 field) */
    for (int i = 0; i < 63; i++)
        xor ^= words[i];
    return xor;
}

static bool mfg_verify_checksum(const struct mfg_data *mfg)
{
    const uint32_t *words = (const uint32_t *)mfg;
    uint32_t xor = 0;
    /* XOR all 64 uint32_t values; result is 0 if checksum is correct */
    for (int i = 0; i < 64; i++)
        xor ^= words[i];
    return xor == 0;
}

static bool parse_date(const char *s, uint32_t *ts)
{
    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int y, m, d;
    if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) return false;
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return false;

    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    if (d > mdays[m - 1] + (m == 2 && leap)) return false;

    uint32_t days = 0;
    for (int i = 1970; i < y; i++)
        days += 365 + (i % 4 == 0 && (i % 100 != 0 || i % 400 == 0));
    for (int i = 0; i < m - 1; i++)
        days += mdays[i] + (i == 1 && leap);
    days += d - 1;

    *ts = days * 86400UL;
    return true;
}

static void format_date(uint32_t ts, char *buf, size_t len)
{
    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (ts == 0) { buf[0] = '\0'; return; }

    uint32_t days = ts / 86400;
    int y = 1970;
    for (;;) {
        int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        if (days < (uint32_t)(365 + leap))
            break;
        days -= 365 + leap;
        y++;
    }
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    int m;
    for (m = 0; m < 12; m++) {
        int md = mdays[m] + (m == 1 && leap);
        if (days < (uint32_t)md)
            break;
        days -= md;
    }
    snprintf(buf, len, "%04d-%02d-%02d", y, m + 1, (int)days + 1);
}

static bool parse_version(const char *s, uint16_t *ver)
{
    unsigned maj, min;
    if (sscanf(s, "%u.%u", &maj, &min) != 2) return false;
    if (maj > 255 || min > 255) return false;
    *ver = (uint16_t)((maj << 8) | min);
    return true;
}

static void format_version(uint16_t ver, char *buf, size_t len)
{
    snprintf(buf, len, "%u.%u", (ver >> 8) & 0xFF, ver & 0xFF);
}

static bool parse_bcd_datecode(const char *s, uint8_t out[8])
{
    memset(out, 0, 8);
    /* Expected format: "YY/WW" where YY and WW are 2-digit numbers */
    unsigned yy, ww;
    if (sscanf(s, "%u/%u", &yy, &ww) != 2) return false;
    if (yy > 99 || ww > 53) return false;
    /* Pack each pair of digits into BCD: e.g. 26 -> 0x26 */
    out[0] = (uint8_t)(((yy / 10) << 4) | (yy % 10));
    out[1] = (uint8_t)(((ww / 10) << 4) | (ww % 10));
    return true;
}

static void format_bcd_datecode(const uint8_t dc[8], char *buf, size_t len)
{
    unsigned yy = ((dc[0] >> 4) & 0x0F) * 10 + (dc[0] & 0x0F);
    unsigned ww = ((dc[1] >> 4) & 0x0F) * 10 + (dc[1] & 0x0F);
    snprintf(buf, len, "%02u/%02u", yy, ww);
}

struct color_alias {
    const char *name;
    uint8_t r, g, b;
};

static const struct color_alias color_aliases[] = {
    { "black",   0x4, 0x4, 0x4 },
    { "green",   0x1, 0xA, 0x3 },
    { "purple",  0x6, 0x2, 0xC },
    { "red",     0xD, 0x2, 0x2 },
    { "white",   0xD, 0xD, 0xD },
    { "blue",    0x6, 0x8, 0xB },
    { NULL, 0, 0, 0 }
};

static bool parse_color(const char *s, uint8_t *rg, uint8_t *b)
{
    /* Check named color aliases first */
    for (const struct color_alias *a = color_aliases; a->name; a++) {
        if (strcmp(s, a->name) == 0) {
            *rg = (uint8_t)((a->r << 4) | a->g);
            *b  = (uint8_t)(a->b & 0x0F);
            return true;
        }
    }

    /* Otherwise parse #RGB hex */
    if (*s == '#') s++;
    if (strlen(s) != 3) return false;
    unsigned r_val, g_val, b_val;
    char tmp[2] = {0, 0};
    tmp[0] = s[0]; r_val = (unsigned)strtoul(tmp, NULL, 16);
    tmp[0] = s[1]; g_val = (unsigned)strtoul(tmp, NULL, 16);
    tmp[0] = s[2]; b_val = (unsigned)strtoul(tmp, NULL, 16);
    *rg = (uint8_t)((r_val << 4) | g_val);
    *b  = (uint8_t)(b_val & 0x0F);
    return true;
}

static void format_color(uint8_t rg, uint8_t b, char *buf, size_t len)
{
    unsigned r = (rg >> 4) & 0x0F;
    unsigned g = rg & 0x0F;
    unsigned bv = b & 0x0F;
    snprintf(buf, len, "#%X%X%X", r, g, bv);
}

/* ===== mfg config file parser ===== */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
        end--;
    *end = '\0';
    return s;
}

static bool parse_mfg_config(const char *path, struct mfg_data *mfg, bool *has_serial)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "open '%s': %s\n", path, strerror(errno)); return false; }

    memset(mfg, 0, sizeof(*mfg));
    mfg->magic = MFG_MAGIC;
    mfg->struct_version = MFG_VERSION;
    *has_serial = false;
    unsigned long raw_serial = 0;

    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = trim(line);
        if (*p == '#' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) {
            fprintf(stderr, "%s:%d: missing '='\n", path, lineno);
            fclose(f);
            return false;
        }
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        if (strcmp(key, "card_type") == 0) {
            strncpy(mfg->card_type, val, sizeof(mfg->card_type) - 1);
        } else if (strcmp(key, "serial") == 0) {
            if (*val) {
                raw_serial = strtoul(val, NULL, 10);
                *has_serial = true;
            }
        } else if (strcmp(key, "hw_revision") == 0) {
            if (!parse_version(val, &mfg->hw_revision)) {
                fprintf(stderr, "%s:%d: bad hw_revision '%s'\n", path, lineno, val);
                fclose(f); return false;
            }
        } else if (strcmp(key, "pcb_color") == 0) {
            if (!parse_color(val, &mfg->pcb_color_rg, &mfg->pcb_color_b)) {
                fprintf(stderr, "%s:%d: bad pcb_color '%s'\n", path, lineno, val);
                fclose(f); return false;
            }
        } else if (strcmp(key, "assembler_name") == 0) {
            strncpy(mfg->assembler_name, val, sizeof(mfg->assembler_name) - 1);
        } else if (strcmp(key, "assembly_factory") == 0) {
            strncpy(mfg->assembly_factory, val, sizeof(mfg->assembly_factory) - 1);
        } else if (strcmp(key, "build_date") == 0) {
            if (*val && !parse_date(val, &mfg->build_date)) {
                fprintf(stderr, "%s:%d: bad build_date '%s'\n", path, lineno, val);
                fclose(f); return false;
            }
        } else if (strcmp(key, "batch_number") == 0) {
            mfg->batch_number = (uint16_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "siop_datecode") == 0) {
            if (*val && !parse_bcd_datecode(val, (uint8_t *)mfg->siop_datecode)) {
                fprintf(stderr, "%s:%d: bad siop_datecode '%s'\n", path, lineno, val);
                fclose(f); return false;
            }
        } else if (strcmp(key, "cpld_version") == 0) {
            if (!parse_version(val, &mfg->cpld_version)) {
                fprintf(stderr, "%s:%d: bad cpld_version '%s'\n", path, lineno, val);
                fclose(f); return false;
            }
        } else if (strcmp(key, "initial_fw_version") == 0) {
            if (!parse_version(val, &mfg->initial_fw_version)) {
                fprintf(stderr, "%s:%d: bad initial_fw_version '%s'\n", path, lineno, val);
                fclose(f); return false;
            }
        } else if (strcmp(key, "test_date") == 0) {
            if (*val && !parse_date(val, &mfg->test_date)) {
                fprintf(stderr, "%s:%d: bad test_date '%s'\n", path, lineno, val);
                fclose(f); return false;
            }
        } else if (strcmp(key, "test_fw_version") == 0) {
            if (!parse_version(val, &mfg->test_fw_version)) {
                fprintf(stderr, "%s:%d: bad test_fw_version '%s'\n", path, lineno, val);
                fclose(f); return false;
            }
        } else if (strcmp(key, "test_status") == 0) {
            mfg->test_status = (uint16_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "owner_name") == 0) {
            strncpy(mfg->owner_name, val, sizeof(mfg->owner_name) - 1);
        } else if (strcmp(key, "sku") == 0) {
            strncpy(mfg->sku, val, sizeof(mfg->sku) - 1);
        } else {
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, lineno, key);
            fclose(f); return false;
        }
    }

    fclose(f);

    /* Compose serial after parsing so card_type is always available */
    if (*has_serial)
        snprintf(mfg->serial, sizeof(mfg->serial), "%s-%08lu", mfg->card_type, raw_serial);

    return true;
}

/* ===== mfg config file writer ===== */

static bool write_mfg_config(const char *path, const struct mfg_data *mfg, bool crc_ok)
{
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "open '%s' for write: %s\n", path, strerror(errno)); return false; }

    if (!crc_ok)
        fprintf(f, "# WARNING: checksum mismatch\n");
    fprintf(f, "# A4092 Manufacturing Data\n");

    /* card_type */
    fprintf(f, "card_type = %s\n", mfg->card_type);

    /* serial: extract the 8-digit number after the card_type prefix */
    const char *dash = strchr(mfg->serial, '-');
    if (dash)
        fprintf(f, "serial = %s\n", dash + 1);
    else
        fprintf(f, "serial = %s\n", mfg->serial);

    /* hw_revision */
    char vbuf[16];
    format_version(mfg->hw_revision, vbuf, sizeof(vbuf));
    fprintf(f, "hw_revision = %s\n", vbuf);

    /* pcb_color */
    char cbuf[8];
    format_color(mfg->pcb_color_rg, mfg->pcb_color_b, cbuf, sizeof(cbuf));
    fprintf(f, "pcb_color = %s\n", cbuf);

    fprintf(f, "assembler_name = %s\n", mfg->assembler_name);
    fprintf(f, "assembly_factory = %s\n", mfg->assembly_factory);

    char dbuf[16];
    format_date(mfg->build_date, dbuf, sizeof(dbuf));
    fprintf(f, "build_date = %s\n", dbuf);
    fprintf(f, "batch_number = %u\n", mfg->batch_number);

    char dcbuf[16];
    format_bcd_datecode((const uint8_t *)mfg->siop_datecode, dcbuf, sizeof(dcbuf));
    fprintf(f, "siop_datecode = %s\n", dcbuf);

    format_version(mfg->cpld_version, vbuf, sizeof(vbuf));
    fprintf(f, "cpld_version = %s\n", vbuf);

    format_version(mfg->initial_fw_version, vbuf, sizeof(vbuf));
    fprintf(f, "initial_fw_version = %s\n", vbuf);

    format_date(mfg->test_date, dbuf, sizeof(dbuf));
    fprintf(f, "test_date = %s\n", dbuf);

    format_version(mfg->test_fw_version, vbuf, sizeof(vbuf));
    fprintf(f, "test_fw_version = %s\n", vbuf);

    fprintf(f, "test_status = 0x%02X\n", mfg->test_status);
    fprintf(f, "owner_name = %s\n", mfg->owner_name);
    fprintf(f, "sku = %s\n", mfg->sku);

    fclose(f);
    return true;
}

/* ===== mfg console dump ===== */

static void print_mfg_data(const struct mfg_data *mfg, bool crc_ok)
{
    char buf[64];

    printf("=== Manufacturing Data ===\n");
    if (!crc_ok)
        printf("WARNING: checksum mismatch (stored 0x%08lX, computed 0x%08lX)\n",
               (unsigned long)mfg->crc32, (unsigned long)mfg_checksum(mfg));

    printf("Card type:          %s\n", mfg->card_type);
    printf("Serial:             %s\n", mfg->serial);

    format_version(mfg->hw_revision, buf, sizeof(buf));
    printf("HW revision:        %s\n", buf);

    format_color(mfg->pcb_color_rg, mfg->pcb_color_b, buf, sizeof(buf));
    printf("PCB color:          %s (RGB888: #%02X%02X%02X)\n", buf,
           PCB_COLOR_R8(mfg), PCB_COLOR_G8(mfg), PCB_COLOR_B8(mfg));

    printf("Assembler:          %s\n", mfg->assembler_name);
    printf("Assembly factory:   %s\n", mfg->assembly_factory);

    format_date(mfg->build_date, buf, sizeof(buf));
    printf("Build date:         %s\n", buf);
    printf("Batch number:       %u\n", mfg->batch_number);

    format_bcd_datecode((const uint8_t *)mfg->siop_datecode, buf, sizeof(buf));
    printf("SIOP date code:     %s\n", buf);

    format_version(mfg->cpld_version, buf, sizeof(buf));
    printf("CPLD version:       %s\n", buf);

    format_version(mfg->initial_fw_version, buf, sizeof(buf));
    printf("Initial FW version: %s\n", buf);

    format_date(mfg->test_date, buf, sizeof(buf));
    printf("Test date:          %s\n", buf);

    format_version(mfg->test_fw_version, buf, sizeof(buf));
    printf("Test FW version:    %s\n", buf);

    printf("Test status:        0x%02X", mfg->test_status);
    if (mfg->test_status) {
        printf(" (");
        const char *sep = "";
        if (mfg->test_status & TEST_PASSED_REGS)  { printf("%sREGS",  sep); sep = " "; }
        if (mfg->test_status & TEST_PASSED_IRQ)   { printf("%sIRQ",   sep); sep = " "; }
        if (mfg->test_status & TEST_PASSED_SCSI)  { printf("%sSCSI",  sep); sep = " "; }
        if (mfg->test_status & TEST_PASSED_DMA)   { printf("%sDMA",   sep); sep = " "; }
        if (mfg->test_status & TEST_PASSED_FLASH) { printf("%sFLASH", sep); sep = " "; }
        if (mfg->test_status & TEST_PASSED_CPLD)  { printf("%sCPLD",  sep); sep = " "; }
        printf(")");
    }
    printf("\n");

    printf("Owner:              %s\n", mfg->owner_name);
    printf("SKU:                %s\n", mfg->sku);
    printf("Checksum:           0x%08lX %s\n", (unsigned long)mfg->crc32,
           crc_ok ? "(OK)" : "(BAD)");
}

/* ===== usage ===== */
static void usage(const char *argv0)
{
    printf(
"Usage:\n"
"  %s id\n"
"  %s uid\n"
"  %s read  <addr> <len> <outfile>\n"
"  %s erase <addr> <len> [--verify]\n"
"  %s write <addr> <infile> [--verify]\n"
"  %s verify <addr> <infile>\n"
"  %s patch <addr> <hexbytes>\n"
"  %s status\n"
"  %s writemfg <config_file>\n"
"  %s readmfg <config_file>\n"
"  %s dumpmfg\n"
"  %s writeoem <infile> [--verify]\n"
"  %s readoem <outfile>\n"
"  %s eraseoem\n"
"  %s dumpoem\n"
"\n"
"Notes:\n"
"  - addr/len accept decimal or 0xHEX; len also supports K/M suffix (e.g. 64K).\n"
"  - 'write' assumes the destination is erased; run 'erase' first.\n"
"  - 'patch' writes a few bytes (e.g. 0xDE,0xAD,0xBE,0xEF) within an erased area.\n"
"  - 'writemfg' programs manufacturing data from a config file.\n"
"  - 'readmfg' reads manufacturing data from flash to a config file.\n"
"  - 'dumpmfg' displays manufacturing data on the console.\n"
"  - 'writeoem' writes a pre-built OEM image blob (from mkoemblob.py) to flash.\n"
"  - 'readoem' reads the OEM image blob from flash to a file.\n"
"  - 'eraseoem' erases the OEM image section.\n"
"  - 'dumpoem' displays OEM image header on the console.\n"
, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

/* parse CSV of hex bytes for 'patch' */
static bool parse_hexbytes(const char *s, uint8_t **out, size_t *outlen)
{
    const size_t max_hex_chars = 4096; /* avoid unbounded input */
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
    case 0x3013:
	    printf("W25X40");
	    break;
    case 0x4013:
	    printf("W25Q40");
	    break;
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

static int cmd_uid(uint32_t base)
{
    uint8_t id[8];
    spi_read_unique_id(base, id);
    printf("Unique ID: %02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X\n",
           id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
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
        if (!spi_write_buf_pagewise(base, addr+done, img+done, chunk, NULL)) {
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
    if (n > MAX_TRANSFER_SIZE) { free(bytes); fprintf(stderr,"patch too large (max %d)\n", MAX_TRANSFER_SIZE); return 2; }
    if (!spi_clear_block_protect(base)) { free(bytes); return 3; }
    printf("Patching %zu byte(s) at 0x%08lX (assumes erased)...\n", n, (unsigned long)addr);
    bool ok = spi_write_buf_pagewise(base, addr, bytes, n, NULL);
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

/* ===== mfg commands ===== */

static int cmd_writemfg(uint32_t base, const char *config_file)
{
    struct mfg_data mfg;
    bool has_serial = false;

    if (!parse_mfg_config(config_file, &mfg, &has_serial))
        return 2;

    /* Handle serial number: from config or auto-increment from serial.txt */
    if (!has_serial) {
        unsigned long serial_num = 1;
        FILE *sf = fopen("S/serial.txt", "r");
        if (sf) {
            if (fscanf(sf, "%lu", &serial_num) != 1)
                serial_num = 1;
            fclose(sf);
        }
        snprintf(mfg.serial, sizeof(mfg.serial), "%s-%08lu", mfg.card_type, serial_num);
        printf("Auto-assigned serial: %s\n", mfg.serial);

        /* Write back incremented serial */
        sf = fopen("S/serial.txt", "w");
        if (sf) {
            fprintf(sf, "%lu\n", serial_num + 1);
            fclose(sf);
        } else {
            fprintf(stderr, "WARNING: could not update serial.txt\n");
        }
    }

    /* Compute checksum */
    mfg.crc32 = mfg_checksum(&mfg);

    printf("Writing manufacturing data to flash at 0x%lX...\n", (unsigned long)MFG_FLASH_OFFSET);

    /* Clear block protection */
    if (!spi_clear_block_protect(base))
        return 3;

    /* Erase 4KB sector */
    printf("Erasing 4KB sector at 0x%lX...\n", (unsigned long)MFG_FLASH_OFFSET);
    if (!spi_sector_erase_4k(base, MFG_FLASH_OFFSET)) {
        fprintf(stderr, "ERROR: 4K sector erase failed\n");
        return 3;
    }

    /* Write 256 bytes */
    if (!spi_write_buf_pagewise(base, MFG_FLASH_OFFSET, (const uint8_t *)&mfg, sizeof(mfg), NULL)) {
        fprintf(stderr, "ERROR: flash write failed\n");
        return 3;
    }

    /* Verify by reading back */
    struct mfg_data verify;
    if (!spi_read_buf(base, MFG_FLASH_OFFSET, (uint8_t *)&verify, sizeof(verify))) {
        fprintf(stderr, "ERROR: flash readback failed\n");
        return 3;
    }
    if (memcmp(&mfg, &verify, sizeof(mfg)) != 0) {
        fprintf(stderr, "ERROR: flash verify failed - data mismatch\n");
        return 4;
    }

    printf("Manufacturing data written and verified OK\n");
    printf("Serial: %s\n", mfg.serial);
    return 0;
}

static int cmd_readmfg(uint32_t base, const char *config_file)
{
    struct mfg_data mfg;

    if (!spi_read_buf(base, MFG_FLASH_OFFSET, (uint8_t *)&mfg, sizeof(mfg))) {
        fprintf(stderr, "ERROR: flash read failed\n");
        return 3;
    }

    if (mfg.magic != MFG_MAGIC) {
        fprintf(stderr, "No manufacturing data found (bad magic: 0x%08lX)\n",
                (unsigned long)mfg.magic);
        return 2;
    }

    bool crc_ok = mfg_verify_checksum(&mfg);
    if (!crc_ok)
        fprintf(stderr, "WARNING: checksum mismatch\n");

    if (!write_mfg_config(config_file, &mfg, crc_ok))
        return 3;

    printf("Manufacturing data written to %s\n", config_file);
    return 0;
}

static int cmd_dumpmfg(uint32_t base)
{
    struct mfg_data mfg;

    if (!spi_read_buf(base, MFG_FLASH_OFFSET, (uint8_t *)&mfg, sizeof(mfg))) {
        fprintf(stderr, "ERROR: flash read failed\n");
        return 3;
    }

    if (mfg.magic != MFG_MAGIC) {
        fprintf(stderr, "No manufacturing data found (bad magic: 0x%08lX)\n",
                (unsigned long)mfg.magic);
        return 2;
    }

    bool crc_ok = mfg_verify_checksum(&mfg);
    print_mfg_data(&mfg, crc_ok);
    return 0;
}

/* ===== oem commands ===== */

static uint32_t oem_checksum(const struct oem_header *h)
{
    const uint32_t *words = (const uint32_t *)h;
    uint32_t xor = 0;

    for (size_t i = 0; i < (offsetof(struct oem_header, checksum) / 4); i++)
        xor ^= words[i];
    return xor;
}

static bool oem_verify_checksum(const struct oem_header *h)
{
    return oem_checksum(h) == h->checksum;
}

static bool oem_variant_present(const struct oem_variant *v)
{
    return v->compressed_size != 0;
}

static int oem_variant_depth(int slot)
{
    return OEM_VARIANT_DEPTH(slot);
}

static bool oem_validate_bundle(const struct oem_header *h, size_t available_size,
                                char *error, size_t error_len)
{
    int present = 0;

    if (available_size < sizeof(*h)) {
        snprintf(error, error_len, "file too small for OEM header (%zu bytes)", available_size);
        return false;
    }
    if (h->magic != OEM_MAGIC) {
        snprintf(error, error_len, "bad OEM magic (0x%08lX)", (unsigned long)h->magic);
        return false;
    }
    if (!OEM_IS_VALID(h)) {
        snprintf(error, error_len, "unsupported OEM bundle header");
        return false;
    }
    if (!oem_verify_checksum(h)) {
        snprintf(error, error_len, "OEM header checksum mismatch");
        return false;
    }
    if (available_size < h->total_size) {
        snprintf(error, error_len, "blob shorter than total_size (%zu < %lu)",
                 available_size, (unsigned long)h->total_size);
        return false;
    }

    for (int slot = 0; slot < OEM_VARIANT_SLOTS; slot++) {
        const struct oem_variant *variant = &h->variant[slot];
        int depth = oem_variant_depth(slot);

        if (!oem_variant_present(variant)) {
            if (variant->width != 0 ||
                variant->height != 0 ||
                variant->uncompressed_size != 0 ||
                variant->data_offset != 0) {
                snprintf(error, error_len, "variant slot %d has partial metadata but no payload", slot);
                return false;
            }
            continue;
        }

        present++;

        if (variant->width == 0 || variant->height == 0) {
            snprintf(error, error_len, "variant slot %d has invalid dimensions %ux%u",
                     slot, variant->width, variant->height);
            return false;
        }
        if (variant->data_offset < sizeof(*h) || variant->data_offset > h->total_size) {
            snprintf(error, error_len, "variant slot %d has invalid data offset 0x%08lX",
                     slot, (unsigned long)variant->data_offset);
            return false;
        }
        if (variant->compressed_size > (h->total_size - variant->data_offset)) {
            snprintf(error, error_len, "variant slot %d payload overruns blob", slot);
            return false;
        }
        if (variant->uncompressed_size != oem_expected_uncompressed_size(variant, depth)) {
            snprintf(error, error_len,
                     "variant slot %d uncompressed size mismatch (%lu != %lu)",
                     slot,
                     (unsigned long)variant->uncompressed_size,
                     (unsigned long)oem_expected_uncompressed_size(variant, depth));
            return false;
        }
    }

    if (present == 0) {
        snprintf(error, error_len, "bundle contains no variants");
        return false;
    }
    if (present != h->variant_count) {
        snprintf(error, error_len, "variant count mismatch (%u != %d)", h->variant_count, present);
        return false;
    }
    return true;
}

static int cmd_writeoem(uint32_t base, const char *infile, bool do_verify)
{
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    char error[160];

    if (!read_file(infile, &blob, &blob_len))
        return 2;

    struct oem_header *h = (struct oem_header *)blob;
    if (!oem_validate_bundle(h, blob_len, error, sizeof(error))) {
        fprintf(stderr, "ERROR: %s\n", error);
        free(blob);
        return 2;
    }
    if (blob_len != h->total_size) {
        fprintf(stderr, "ERROR: file size mismatch (header says %lu bytes, file is %zu)\n",
                (unsigned long)h->total_size, blob_len);
        free(blob);
        return 2;
    }

    printf("OEM bundle: %u variant(s), %lu bytes total\n",
           h->variant_count, (unsigned long)h->total_size);
    for (int slot = 0; slot < OEM_VARIANT_SLOTS; slot++) {
        const struct oem_variant *variant = &h->variant[slot];
        int depth = oem_variant_depth(slot);

        if (!oem_variant_present(variant))
            continue;

        printf("  %d bitplanes: %ux%u at x=",
               depth, variant->width, variant->height);
        if (variant->x == OEM_COORD_CENTER)
            printf("center");
        else
            printf("%u", variant->x);
        printf(" y=");
        if (variant->y == OEM_COORD_CENTER)
            printf("center");
        else
            printf("%u", variant->y);
        printf(", %lu -> %lu bytes\n",
               (unsigned long)variant->uncompressed_size,
               (unsigned long)variant->compressed_size);
    }

    if (!spi_clear_block_protect(base)) { free(blob); return 3; }

    /* Erase OEM region in 4KB sectors */
    printf("Erasing OEM region (0x%X, %u KB)...\n", OEM_FLASH_OFFSET, OEM_FLASH_SIZE / 1024);
    for (uint32_t addr = OEM_FLASH_OFFSET; addr < OEM_FLASH_OFFSET + OEM_FLASH_SIZE; addr += SPI_SECTOR_SIZE_4K) {
        if (!spi_sector_erase_4k(base, addr)) {
            fprintf(stderr, "ERROR: erase failed at 0x%08lX\n", (unsigned long)addr);
            free(blob);
            return 3;
        }
        bar_progress(addr - OEM_FLASH_OFFSET + SPI_SECTOR_SIZE_4K, OEM_FLASH_SIZE, "erase");
    }

    /* Write blob */
    printf("Writing %zu bytes to 0x%X...\n", blob_len, OEM_FLASH_OFFSET);
    size_t done = 0;
    while (done < blob_len) {
        size_t chunk = (blob_len - done) > 4096 ? 4096 : (blob_len - done);
        if (!spi_write_buf_pagewise(base, OEM_FLASH_OFFSET + done, blob + done, chunk, NULL)) {
            fprintf(stderr, "ERROR: write failed at +0x%zx\n", done);
            free(blob);
            return 3;
        }
        done += chunk;
        bar_progress(done, blob_len, "write");
    }

    if (do_verify) {
        printf("Verifying...\n");
        bool ok = spi_verify_buf(base, OEM_FLASH_OFFSET, blob, blob_len, bar_cb_verify);
        free(blob);
        if (!ok) { fprintf(stderr, "Verify FAILED\n"); return 4; }
        printf("Verify OK\n");
    } else {
        free(blob);
    }

    printf("Write OK\n");
    return 0;
}

static int cmd_readoem(uint32_t base, const char *outfile)
{
    struct oem_header h;
    char error[160];

    if (!spi_read_buf(base, OEM_FLASH_OFFSET, (uint8_t *)&h, sizeof(h))) {
        fprintf(stderr, "ERROR: flash read failed\n");
        return 3;
    }
    if (h.magic != OEM_MAGIC) {
        fprintf(stderr, "No OEM data found (bad magic: 0x%08lX)\n", (unsigned long)h.magic);
        return 2;
    }
    if (!oem_validate_bundle(&h, OEM_FLASH_SIZE, error, sizeof(error))) {
        fprintf(stderr, "ERROR: %s\n", error);
        return 2;
    }

    size_t blob_len = h.total_size;
    uint8_t *buf = (uint8_t *)malloc(blob_len);
    if (!buf) { fprintf(stderr, "OOM\n"); return 2; }

    if (!spi_read_buf(base, OEM_FLASH_OFFSET, buf, blob_len)) {
        fprintf(stderr, "ERROR: flash read failed\n");
        free(buf);
        return 3;
    }

    if (!write_file(outfile, buf, blob_len)) {
        free(buf);
        return 3;
    }

    free(buf);
    printf("OEM data (%zu bytes) written to %s\n", blob_len, outfile);
    return 0;
}

static int cmd_eraseoem(uint32_t base)
{
    if (!spi_clear_block_protect(base))
        return 3;

    printf("Erasing OEM region (0x%X, %u KB)...\n", OEM_FLASH_OFFSET, OEM_FLASH_SIZE / 1024);
    for (uint32_t addr = OEM_FLASH_OFFSET; addr < OEM_FLASH_OFFSET + OEM_FLASH_SIZE; addr += SPI_SECTOR_SIZE_4K) {
        if (!spi_sector_erase_4k(base, addr)) {
            fprintf(stderr, "ERROR: erase failed at 0x%08lX\n", (unsigned long)addr);
            return 3;
        }
        bar_progress(addr - OEM_FLASH_OFFSET + SPI_SECTOR_SIZE_4K, OEM_FLASH_SIZE, "erase");
    }

    printf("Verifying erase...\n");
    bool ok = spi_verify_erased(base, OEM_FLASH_OFFSET, OEM_FLASH_SIZE, bar_cb_erase_verify);
    if (!ok) { fprintf(stderr, "Erase verify FAILED\n"); return 4; }

    printf("Erase OK\n");
    return 0;
}

static int cmd_dumpoem(uint32_t base)
{
    struct oem_header h;
    char error[160];

    if (!spi_read_buf(base, OEM_FLASH_OFFSET, (uint8_t *)&h, sizeof(h))) {
        fprintf(stderr, "ERROR: flash read failed\n");
        return 3;
    }

    if (h.magic != OEM_MAGIC) {
        fprintf(stderr, "No OEM data found (bad magic: 0x%08lX)\n", (unsigned long)h.magic);
        return 2;
    }
    if (!oem_validate_bundle(&h, OEM_FLASH_SIZE, error, sizeof(error))) {
        fprintf(stderr, "ERROR: %s\n", error);
        return 2;
    }

    printf("=== OEM Image Bundle ===\n");
    printf("Version:            %u\n", h.version);
    printf("Variants present:   %u\n", h.variant_count);
    printf("Total size:         %lu bytes\n", (unsigned long)h.total_size);
    printf("Checksum:           0x%08lX (OK)\n", (unsigned long)h.checksum);

    for (int slot = 0; slot < OEM_VARIANT_SLOTS; slot++) {
        const struct oem_variant *variant = &h.variant[slot];
        int depth = oem_variant_depth(slot);
        int num_colors = 1 << depth;

        if (!oem_variant_present(variant))
            continue;

        printf("\n[%d bitplanes]\n", depth);
        printf("Dimensions:         %ux%u\n", variant->width, variant->height);
        printf("X:                  %s", variant->x == OEM_COORD_CENTER ? "center" : "");
        if (variant->x != OEM_COORD_CENTER)
            printf("%u", variant->x);
        printf("\n");
        printf("Y:                  %s", variant->y == OEM_COORD_CENTER ? "center" : "");
        if (variant->y != OEM_COORD_CENTER)
            printf("%u", variant->y);
        printf("\n");
        printf("Data offset:        0x%08lX\n", (unsigned long)variant->data_offset);
        printf("Compressed size:    %lu bytes\n", (unsigned long)variant->compressed_size);
        printf("Uncompressed size:  %lu bytes\n", (unsigned long)variant->uncompressed_size);
        printf("Compression ratio:  %lu%%\n",
               (unsigned long)(variant->compressed_size * 100 / variant->uncompressed_size));
        printf("Palette:");
        for (int i = 0; i < num_colors; i++) {
            if (i % 8 == 0)
                printf("\n  ");
            printf("#%03X ", variant->palette[i] & 0xFFF);
        }
        printf("\n");
    }

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
    int unsupported_a4091_found = 0;
    if ((ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library",0)) != NULL) {
        while ((cd = FindConfigDev(cd,-1,-1)) != NULL) {
            uint32_t candidate_base = (uint32_t)(uintptr_t)cd->cd_BoardAddr;

            if (board_has_a4092_id(cd) || (board_has_legacy_a409x_id(cd) &&
                                           a4092_spi_flash_present(candidate_base))) {
                base = candidate_base;
                boards_found++;
                break;
            }

            if (board_has_legacy_a409x_id(cd))
                unsupported_a4091_found = 1;
        }
        CloseLibrary((struct Library *)ExpansionBase);
        ExpansionBase = NULL;
    }

    if (!base || boards_found == 0) {
        if (unsupported_a4091_found)
            fprintf(stderr, "Found A4091-compatible board(s), but mfgtool only works on A4092 SPI hardware\n");
        else
            fprintf(stderr, "No A4092 board found via expansion.library\n");
        return 1;
    } else {
	printf("A4092 found at 0x%"PRIx32"\n", base);
    }

    int argi = 1;
    const char *cmd = argv[argi++];

    if (strcmp(cmd,"id")==0) {
        return cmd_id(base);
    }
    else if (strcmp(cmd,"uid")==0) {
        return cmd_uid(base);
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
    else if (strcmp(cmd,"writemfg")==0) {
        if (argi >= argc) { usage(argv[0]); return 1; }
        return cmd_writemfg(base, argv[argi]);
    }
    else if (strcmp(cmd,"readmfg")==0) {
        if (argi >= argc) { usage(argv[0]); return 1; }
        return cmd_readmfg(base, argv[argi]);
    }
    else if (strcmp(cmd,"dumpmfg")==0) {
        return cmd_dumpmfg(base);
    }
    else if (strcmp(cmd,"writeoem")==0) {
        if (argi >= argc) { usage(argv[0]); return 1; }
        bool verify = false;
        if (argi+1 < argc && strcmp(argv[argi+1],"--verify")==0) verify = true;
        return cmd_writeoem(base, argv[argi], verify);
    }
    else if (strcmp(cmd,"readoem")==0) {
        if (argi >= argc) { usage(argv[0]); return 1; }
        return cmd_readoem(base, argv[argi]);
    }
    else if (strcmp(cmd,"eraseoem")==0) {
        return cmd_eraseoem(base);
    }
    else if (strcmp(cmd,"dumpoem")==0) {
        return cmd_dumpoem(base);
    }

    usage(argv[0]);
    return 1;
}
