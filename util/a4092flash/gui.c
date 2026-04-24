// SPDX-License-Identifier: GPL-2.0-only

#include <string.h>
#include <stdio.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <utility/tagitem.h>
#include <graphics/displayinfo.h>

#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>

#include "flash.h"
#include "gui.h"
#include "hardware.h"
#include "main.h"
#include "oem_flash.h"

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase *GfxBase = NULL;
struct Library *GadToolsBase = NULL;
struct Library *AslBase = NULL;

struct UiContext {
  struct Window *window;
  struct Screen *screen;
  APTR visualInfo;
  BOOL lockedPubScreen;
  BOOL gadgetsAdded;
  struct Gadget *gadgets;
  struct Gadget *cardValueGad;
  struct Gadget *flashValueGad;
  struct Gadget *bufferValueGad;
  struct Gadget *statusValueGad;
  struct Gadget *loadButtonGad;
  struct Gadget *eraseButtonGad;
  struct Gadget *programButtonGad;
  struct Gadget *mfgButtonGad;
  UBYTE *buffer;
  ULONG bufferSize;
  ULONG progressPercent;
  ULONG progressDrawnPercent;
  BOOL progressDrawn;
  BOOL hasMfgData;
  char bufferPath[256];
  char cardText[160];
  char flashText[160];
  char bufferText[160];
  char statusText[160];
  struct boardInfo board;
  struct romInfo installedRom;
  struct romInfo bufferedRom;
};

enum {
  GID_LOAD_ROM = 100,
  GID_ERASE_FLASH,
  GID_PROGRAM_FLASH,
  GID_MANUFACTURING,
  GID_ABOUT,
  GID_QUIT,
  GID_ROM_PATH,
  GID_ROM_PATH_LOAD,
  GID_ROM_PATH_CANCEL
};

extern struct ExpansionBase *ExpansionBase;

#define UI_TOP_ROW 24
#define UI_ROW_STEP 22
#define UI_LABEL_X 10
#define UI_VALUE_X 127
#define UI_FIELD_Y_ADJUST 2
#define UI_LABEL_WIDTH 112
#define UI_FULL_FIELD_WIDTH 425
#define UI_BUFFER_FIELD_WIDTH 315
#define UI_BUTTON_X 445
#define UI_SMALL_BUTTON_WIDTH 100
#define UI_PROGRESS_HEIGHT 16
#define UI_PROGRESS_MAX 100
#define UI_PROGRESS_BG_PEN 0
#define UI_PROGRESS_FG_PEN 3
#define UI_PROGRESS_TEXT_PEN 1
#define UI_MIN_ROM_ERASE_SIZE 65536
#define UI_WINDOW_HEIGHT 168

static BOOL ui_begin_flash_access(void)
{
#if SHARED_REGISTERS
    if (!inhibitDosDevs(true))
        return FALSE;
#endif
    return TRUE;
}

static void ui_end_flash_access(void)
{
#if SHARED_REGISTERS
    inhibitDosDevs(false);
#endif
}

static void ui_free_buffer(struct UiContext *ctx)
{
    if (ctx->buffer) {
        FreeMem(ctx->buffer, ctx->bufferSize);
        ctx->buffer = NULL;
    }
    ctx->bufferSize = 0;
    ctx->bufferPath[0] = '\0';
    memset(&ctx->bufferedRom, 0, sizeof(ctx->bufferedRom));
    strcpy(ctx->bufferedRom.summary, "Empty");
}

static LONG ui_request(struct Window *window, const char *title, const char *body,
                       const char *gadgets)
{
    struct EasyStruct es;

    es.es_StructSize = sizeof(es);
    es.es_Flags = 0;
    es.es_Title = (STRPTR)title;
    es.es_TextFormat = (STRPTR)body;
    es.es_GadgetFormat = (STRPTR)gadgets;

    return EasyRequestArgs(window, &es, NULL, NULL);
}

static void ui_set_open_error(char *error, size_t error_len, const char *message)
{
    if (!error || error_len == 0)
        return;

    strncpy(error, message, error_len - 1);
    error[error_len - 1] = '\0';
}

static void ui_append_missing_component(char *buf, size_t buf_len,
                                        const char *component)
{
    size_t used;

    if (!buf || buf_len == 0 || !component)
        return;

    used = strlen(buf);
    if (used >= buf_len - 1)
        return;

    if (used != 0) {
        strncpy(buf + used, ", ", buf_len - used - 1);
        buf[buf_len - 1] = '\0';
        used = strlen(buf);
        if (used >= buf_len - 1)
            return;
    }

    strncpy(buf + used, component, buf_len - used - 1);
    buf[buf_len - 1] = '\0';
}

static void ui_set_text_gadget(struct Gadget *gad, struct Window *window, char *text)
{
    struct TagItem tags[] = {
        { GTTX_Text, (ULONG)text },
        { TAG_DONE, 0 }
    };

    GT_SetGadgetAttrsA(gad, window, NULL, tags);
}

static void ui_set_disabled_gadget(struct Gadget *gad, struct Window *window, BOOL disabled)
{
    struct TagItem tags[] = {
        { GA_Disabled, disabled ? TRUE : FALSE },
        { TAG_DONE, 0 }
    };

    GT_SetGadgetAttrsA(gad, window, NULL, tags);
}

static ULONG ui_progress_fill_width(ULONG percent, ULONG innerWidth)
{
    ULONG fillWidth = 0;

    if (percent > UI_PROGRESS_MAX)
        percent = UI_PROGRESS_MAX;

    if (percent > 0) {
        fillWidth = (percent * innerWidth) / UI_PROGRESS_MAX;
        if (fillWidth == 0)
            fillWidth = 1;
    }

    return fillWidth;
}

static void ui_fill_progress_span(struct RastPort *rp, WORD left, WORD top,
                                  WORD right, WORD bottom, ULONG pen)
{
    if (right < left || bottom < top)
        return;

    SetAPen(rp, pen);
    RectFill(rp, left, top, right, bottom);
}

static void ui_clear_progress_text_area(struct RastPort *rp, WORD innerLeft,
                                        WORD innerTop, WORD innerBottom,
                                        ULONG innerWidth, ULONG fillWidth)
{
    char maxText[] = "100%";
    WORD innerRight = innerLeft + (WORD)innerWidth - 1;
    WORD textWidth = TextLength(rp, maxText, (UWORD)strlen(maxText));
    WORD clearLeft = innerLeft + (WORD)((innerWidth - textWidth) / 2) - 1;
    WORD clearRight = clearLeft + textWidth + 1;
    WORD clearTop = innerTop + (WORD)(((innerBottom - innerTop + 1) -
                                      rp->TxHeight) / 2);
    WORD clearBottom = clearTop + rp->TxHeight - 1;
    WORD fillRight = innerLeft + (WORD)fillWidth - 1;

    if (clearLeft < innerLeft)
        clearLeft = innerLeft;
    if (clearRight > innerRight)
        clearRight = innerRight;

    if (fillWidth > 0 && clearLeft <= fillRight) {
        WORD right = (clearRight < fillRight) ? clearRight : fillRight;
        ui_fill_progress_span(rp, clearLeft, clearTop, right, clearBottom,
                              UI_PROGRESS_FG_PEN);
    }

    if (clearRight > fillRight) {
        WORD left = (fillRight + 1 > clearLeft) ? fillRight + 1 : clearLeft;
        ui_fill_progress_span(rp, left, clearTop, clearRight, clearBottom,
                              UI_PROGRESS_BG_PEN);
    }
}

static void ui_draw_progress_text(struct UiContext *ctx, struct RastPort *rp,
                                  WORD innerLeft, WORD innerTop,
                                  WORD innerBottom, ULONG innerWidth)
{
    char percentText[8];
    WORD innerHeight = (WORD)(innerBottom - innerTop + 1);
    WORD textX;
    WORD textY;
    UWORD textLen;

    snprintf(percentText, sizeof(percentText), "%3lu%%",
             (unsigned long)ctx->progressPercent);
    textLen = (UWORD)strlen(percentText);
    textX = innerLeft + (WORD)((innerWidth -
                                TextLength(rp, percentText, textLen)) / 2);
    textY = innerTop + (WORD)((innerHeight - rp->TxHeight) / 2) +
            rp->TxBaseline;
    SetDrMd(rp, JAM1);
    SetAPen(rp, UI_PROGRESS_TEXT_PEN);
    Move(rp, textX, textY);
    Text(rp, percentText, textLen);
}

static void ui_draw_progress(struct UiContext *ctx, BOOL fullRedraw)
{
    struct RastPort *rp;
    WORD progressRow = UI_TOP_ROW + (3 * UI_ROW_STEP);
    WORD left = UI_VALUE_X;
    WORD top = progressRow - UI_FIELD_Y_ADJUST;
    WORD width = UI_FULL_FIELD_WIDTH;
    WORD height = UI_PROGRESS_HEIGHT;
    WORD innerLeft = left + 2;
    WORD innerTop = top + 2;
    WORD innerRight = left + width - 3;
    WORD innerBottom = top + height - 3;
    ULONG innerWidth;
    ULONG fillWidth;
    ULONG oldFillWidth;

    if (!ctx->window || !ctx->visualInfo)
        return;

    rp = ctx->window->RPort;

    if (innerRight < innerLeft || innerBottom < innerTop)
        return;

    innerWidth = (ULONG)(innerRight - innerLeft + 1);
    fillWidth = ui_progress_fill_width(ctx->progressPercent, innerWidth);

    if (fullRedraw || !ctx->progressDrawn ||
        ctx->progressPercent < ctx->progressDrawnPercent) {
        DrawBevelBox(rp, left, top, width, height,
                     GT_VisualInfo, (ULONG)ctx->visualInfo,
                     GTBB_Recessed, TRUE,
                     GTBB_FrameType, BBFT_RIDGE,
                     TAG_DONE);
        ui_fill_progress_span(rp, innerLeft, innerTop, innerRight,
                              innerBottom, UI_PROGRESS_BG_PEN);

        if (fillWidth > 0) {
            ui_fill_progress_span(rp, innerLeft, innerTop,
                                  innerLeft + (WORD)fillWidth - 1,
                                  innerBottom, UI_PROGRESS_FG_PEN);
        }
    } else {
        oldFillWidth = ui_progress_fill_width(ctx->progressDrawnPercent,
                                              innerWidth);
        if (fillWidth > oldFillWidth) {
            ui_fill_progress_span(rp, innerLeft + (WORD)oldFillWidth,
                                  innerTop,
                                  innerLeft + (WORD)fillWidth - 1,
                                  innerBottom, UI_PROGRESS_FG_PEN);
        }
    }

    ui_clear_progress_text_area(rp, innerLeft, innerTop, innerBottom,
                                innerWidth, fillWidth);
    ui_draw_progress_text(ctx, rp, innerLeft, innerTop, innerBottom,
                          innerWidth);
    ctx->progressDrawnPercent = ctx->progressPercent;
    ctx->progressDrawn = TRUE;
}

static void ui_set_status(struct UiContext *ctx, const char *message)
{
    char status[sizeof(ctx->statusText)];
    size_t len = 0;

    if (!ctx || !message)
        return;

    while (*message == '\n' || *message == '\r')
        message++;

    while (message[len] != '\0' && message[len] != '\n' && message[len] != '\r' &&
           len < (sizeof(status) - 1)) {
        status[len] = message[len];
        len++;
    }

    while (len > 0 && status[len - 1] == ' ')
        len--;

    status[len] = '\0';
    if (len == 0)
        return;

    if (strcmp(ctx->statusText, status) == 0)
        return;

    strncpy(ctx->statusText, status, sizeof(ctx->statusText) - 1);
    ctx->statusText[sizeof(ctx->statusText) - 1] = '\0';

    if (!ctx->window || !ctx->statusValueGad)
        return;

    ui_set_text_gadget(ctx->statusValueGad, ctx->window, ctx->statusText);
}

static void ui_status_sink(void *sinkCtx, const char *message)
{
    ui_set_status((struct UiContext *)sinkCtx, message);
}

static void ui_set_default_rom_target(enum boardType type, char *drawer,
                                      size_t drawer_len, char *file,
                                      size_t file_len)
{
    strncpy(drawer, "SYS:ROMs", drawer_len - 1);
    drawer[drawer_len - 1] = '\0';

    switch (type) {
        case BOARD_A4770:
            strncpy(file, "a4770_cdfs.rom", file_len - 1);
            break;
        case BOARD_A4091:
        case BOARD_A4092:
            strncpy(file, "a4092_cdfs.rom", file_len - 1);
            break;
        default:
            strncpy(file, "a4092_cdfs.rom", file_len - 1);
            break;
    }

    file[file_len - 1] = '\0';
}

static void ui_set_default_rom_path(enum boardType type, char *path,
                                    size_t path_len)
{
    char drawer[128];
    char file[64];

    ui_set_default_rom_target(type, drawer, sizeof(drawer), file,
                              sizeof(file));
    strncpy(path, drawer, path_len - 1);
    path[path_len - 1] = '\0';
    AddPart(path, file, path_len);
}

static void ui_copy_mfg_field(char *dest, size_t dest_len, const char *src,
                              size_t src_len)
{
    size_t copy_len = src_len;

    if (copy_len >= dest_len)
        copy_len = dest_len - 1;

    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
}

static void ui_format_mfg_date(uint32_t ts, char *buf, size_t len)
{
    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t days = ts / 86400;
    int y = 1970;
    int m;
    int leap;

    if (ts == 0) {
        buf[0] = '\0';
        return;
    }

    for (;;) {
        leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        if (days < (uint32_t)(365 + leap))
            break;
        days -= 365 + leap;
        y++;
    }

    leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    for (m = 0; m < 12; m++) {
        int md = mdays[m] + (m == 1 && leap);
        if (days < (uint32_t)md)
            break;
        days -= md;
    }

    snprintf(buf, len, "%04d-%02d-%02lu", y, m + 1, (unsigned long)days + 1);
}

static void ui_format_mfg_summary(char *buf, size_t buf_len,
                                  const struct mfg_data *mfg, BOOL checksum_ok)
{
    char card_type[sizeof(mfg->card_type) + 1];
    char serial[sizeof(mfg->serial) + 1];
    char sku[sizeof(mfg->sku) + 1];
    char owner[sizeof(mfg->owner_name) + 1];
    char builder[sizeof(mfg->assembler_name) + 1];
    char factory[sizeof(mfg->assembly_factory) + 1];
    char build_date[16];

    ui_copy_mfg_field(card_type, sizeof(card_type), mfg->card_type,
                      sizeof(mfg->card_type));
    ui_copy_mfg_field(serial, sizeof(serial), mfg->serial, sizeof(mfg->serial));
    ui_copy_mfg_field(sku, sizeof(sku), mfg->sku, sizeof(mfg->sku));
    ui_copy_mfg_field(owner, sizeof(owner), mfg->owner_name,
                      sizeof(mfg->owner_name));
    ui_copy_mfg_field(builder, sizeof(builder), mfg->assembler_name,
                      sizeof(mfg->assembler_name));
    ui_copy_mfg_field(factory, sizeof(factory), mfg->assembly_factory,
                      sizeof(mfg->assembly_factory));
    ui_format_mfg_date(mfg->build_date, build_date, sizeof(build_date));

    snprintf(buf, buf_len,
             "Card: %s\n"
             "Serial: %s\n"
             "SKU: %s\n"
             "Builder: %s\n"
             "Factory: %s\n"
             "Build date: %s\n"
             "HW Rev: %u.%u\n"
             "CPLD: %u.%u\n"
             "Initial FW: %u.%u\n"
             "Owner: %s\n"
             "Test Status: 0x%04X\n"
             "Checksum: %s",
             card_type[0] ? card_type : "-",
             serial[0] ? serial : "-",
             sku[0] ? sku : "-",
             builder[0] ? builder : "-",
             factory[0] ? factory : "-",
             build_date[0] ? build_date : "-",
             (unsigned)MFG_HW_REV_MAJOR(mfg),
             (unsigned)MFG_HW_REV_MINOR(mfg),
             (unsigned)(mfg->cpld_version >> 8),
             (unsigned)(mfg->cpld_version & 0xFF),
             (unsigned)(mfg->initial_fw_version >> 8),
             (unsigned)(mfg->initial_fw_version & 0xFF),
             owner[0] ? owner : "-",
             (unsigned)mfg->test_status,
             checksum_ok ? "OK" : "BAD");
}

static void ui_scan_board(struct UiContext *ctx)
{
    struct ConfigDev *cd = NULL;
    struct mfg_data mfg;

    memset(&ctx->board, 0, sizeof(ctx->board));
    memset(&ctx->installedRom, 0, sizeof(ctx->installedRom));
    ctx->hasMfgData = FALSE;
    strcpy(ctx->installedRom.summary, "No flash detected");

    flash_cleanup();

    if (!ExpansionBase)
        return;

    while ((cd = FindConfigDev(cd, -1, -1)) != NULL) {
        enum boardType detected = board_type_from_configdev(cd);

        if (detected == BOARD_NONE)
            continue;

        ctx->board.present = TRUE;
        ctx->board.type = detected;
        ctx->board.board.cd = cd;
        setup_a4092_board(&ctx->board.board);
        ctx->board.flashAvailable = flash_init(&ctx->board.manufId,
                                               &ctx->board.devId,
                                               ctx->board.board.flashbase,
                                               &ctx->board.flashSize,
                                               &ctx->board.sectorSize);

        if (ctx->board.type == BOARD_A4091 && ctx->board.flashAvailable)
            ctx->board.type = BOARD_A4092;

        break;
    }

    if (ctx->board.flashAvailable)
        inspect_flash_contents(ctx->board.flashSize, &ctx->installedRom);
    else if (ctx->board.present)
        strcpy(ctx->installedRom.summary, "Flash programming not available");

    if (ctx->board.flashAvailable)
        ctx->hasMfgData = read_mfg_data(&mfg, NULL);
}

static void ui_refresh_state(struct UiContext *ctx)
{
    BOOL can_flash = ctx->board.flashAvailable;
    BOOL can_program = can_flash && ctx->buffer && ctx->bufferSize > 0;
    BOOL can_mfg = can_flash && ctx->hasMfgData;

    snprintf(ctx->cardText, sizeof(ctx->cardText), "%.159s",
             board_type_display_name(ctx->board.present ? ctx->board.type :
                                     BOARD_NONE));
    snprintf(ctx->flashText, sizeof(ctx->flashText), "%.159s",
             ctx->installedRom.summary[0] ? ctx->installedRom.summary : "Unknown");
    snprintf(ctx->bufferText, sizeof(ctx->bufferText), "%.159s",
             ctx->buffer && ctx->bufferSize ? ctx->bufferedRom.summary : "Empty");

    if (!ctx->window)
        return;

    ui_set_text_gadget(ctx->cardValueGad, ctx->window, ctx->cardText);
    ui_set_text_gadget(ctx->flashValueGad, ctx->window, ctx->flashText);
    ui_set_text_gadget(ctx->bufferValueGad, ctx->window, ctx->bufferText);
    ui_set_text_gadget(ctx->statusValueGad, ctx->window, ctx->statusText);
    ui_set_disabled_gadget(ctx->eraseButtonGad, ctx->window, !can_flash);
    ui_set_disabled_gadget(ctx->programButtonGad, ctx->window, !can_program);
    ui_set_disabled_gadget(ctx->mfgButtonGad, ctx->window, !can_mfg);
    ui_draw_progress(ctx, TRUE);
}

static struct Gadget *ui_add_text_gadget(struct Gadget *last,
                                         struct NewGadget *ng, STRPTR text,
                                         BOOL bordered)
{
    struct TagItem tags[3];
    UWORD tag_idx = 0;

    tags[tag_idx].ti_Tag = GTTX_Text;
    tags[tag_idx++].ti_Data = (ULONG)text;
    if (bordered) {
        tags[tag_idx].ti_Tag = GTTX_Border;
        tags[tag_idx++].ti_Data = TRUE;
    }
    tags[tag_idx].ti_Tag = TAG_DONE;
    tags[tag_idx].ti_Data = 0;

    return CreateGadgetA(TEXT_KIND, last, ng, tags);
}

static struct Gadget *ui_add_button_gadget(struct Gadget *last,
                                           struct NewGadget *ng, BOOL disabled)
{
    struct TagItem tags[2];
    UWORD tag_idx = 0;

    if (disabled) {
        tags[tag_idx].ti_Tag = GA_Disabled;
        tags[tag_idx++].ti_Data = TRUE;
    }
    tags[tag_idx].ti_Tag = TAG_DONE;
    tags[tag_idx].ti_Data = 0;

    return CreateGadgetA(BUTTON_KIND, last, ng, tags);
}

static struct Gadget *ui_add_string_gadget(struct Gadget *last,
                                           struct NewGadget *ng, STRPTR text,
                                           UWORD max_chars)
{
    struct TagItem tags[] = {
        { GTST_String, (ULONG)text },
        { GTST_MaxChars, max_chars },
        { TAG_DONE, 0 }
    };

    return CreateGadgetA(STRING_KIND, last, ng, tags);
}

static void ui_set_progress(struct UiContext *ctx, ULONG percent)
{
    if (percent > UI_PROGRESS_MAX)
        percent = UI_PROGRESS_MAX;

    if (ctx->progressPercent == percent)
        return;

    ctx->progressPercent = percent;

    if (!ctx->window)
        return;

    ui_draw_progress(ctx, FALSE);
}

static void ui_set_progress_fraction(struct UiContext *ctx, ULONG done,
                                     ULONG total, ULONG startPercent,
                                     ULONG endPercent)
{
    ULONG percent = endPercent;

    if (total > 0 && endPercent >= startPercent) {
        percent = startPercent +
                  ((done * (endPercent - startPercent)) / total);
    }

    ui_set_progress(ctx, percent);
}

static ULONG ui_rom_region_size(const struct UiContext *ctx)
{
    ULONG romRegionSize = ctx->board.flashSize;

    if (romRegionSize > OEM_FLASH_OFFSET)
        romRegionSize = OEM_FLASH_OFFSET;

    return romRegionSize;
}

static ULONG ui_program_erase_size(const struct UiContext *ctx)
{
    ULONG eraseSize = ctx->bufferSize;
    ULONG romRegionSize = ui_rom_region_size(ctx);

    if (eraseSize < UI_MIN_ROM_ERASE_SIZE)
        eraseSize = UI_MIN_ROM_ERASE_SIZE;

    if (ctx->board.sectorSize && (eraseSize % ctx->board.sectorSize) != 0) {
        eraseSize = ((eraseSize + ctx->board.sectorSize - 1) /
                     ctx->board.sectorSize) * ctx->board.sectorSize;
    }

    if (eraseSize > romRegionSize)
        eraseSize = romRegionSize;

    return eraseSize;
}

struct UiEraseProgress {
    struct UiContext *ctx;
    ULONG totalSize;
    ULONG baseDone;
    ULONG chunkSize;
    ULONG startPercent;
    ULONG endPercent;
};

static void ui_erase_progress(void *progressCtx, ULONG done, ULONG total)
{
    struct UiEraseProgress *progress = (struct UiEraseProgress *)progressCtx;
    ULONG overallDone;

    if (!progress || !progress->ctx)
        return;

    overallDone = progress->baseDone;
    if (progress->chunkSize > 0) {
        if (total > 0) {
            overallDone += (ULONG)(((unsigned long long)done *
                                    (unsigned long long)progress->chunkSize) /
                                   (unsigned long long)total);
        } else {
            overallDone += progress->chunkSize;
        }
    }

    ui_set_progress_fraction(progress->ctx, overallDone, progress->totalSize,
                             progress->startPercent, progress->endPercent);
}

static BOOL ui_erase_region(struct UiContext *ctx, ULONG address, ULONG size,
                            ULONG startPercent, ULONG endPercent)
{
    ULONG chunkSize = ctx->board.sectorSize;
    ULONG offset = 0;
    struct UiEraseProgress progress;

    if (size == 0 || chunkSize == 0)
        return FALSE;

    progress.ctx = ctx;
    progress.totalSize = size;
    progress.startPercent = startPercent;
    progress.endPercent = endPercent;

    while (offset < size) {
        ULONG currentSize = chunkSize;

        if (currentSize > (size - offset))
            currentSize = size - offset;

        progress.baseDone = offset;
        progress.chunkSize = currentSize;
        if (!flash_erase_sector_with_progress(address + offset, currentSize,
                                              ui_erase_progress, &progress))
            return FALSE;

        offset += currentSize;
        ui_set_progress_fraction(ctx, offset, size, startPercent, endPercent);
    }

    return TRUE;
}

static void ui_program_write_progress(void *progressCtx,
                                      enum flashWritePhase phase, ULONG done,
                                      ULONG total)
{
    struct UiContext *ctx = (struct UiContext *)progressCtx;

    if (!ctx)
        return;

    if (phase == FLASH_WRITE_PHASE_PROGRAM) {
        ui_set_progress_fraction(ctx, done, total, 25, 70);
    } else {
        ui_set_progress_fraction(ctx, done, total, 70, 100);
    }
}

static BOOL ui_build_gadgets(struct UiContext *ctx)
{
    struct Gadget *last = NULL;
    struct NewGadget ng;
    WORD cardRow = UI_TOP_ROW;
    WORD installedRow = cardRow + UI_ROW_STEP;
    WORD bufferRow = installedRow + UI_ROW_STEP;
    WORD progressRow = bufferRow + UI_ROW_STEP;
    WORD statusRow = progressRow + UI_ROW_STEP;
    WORD buttonRow = statusRow + 28;

    ctx->gadgets = CreateContext(&last);
    if (!ctx->gadgets)
        return FALSE;

    ng.ng_TextAttr = NULL;
    ng.ng_VisualInfo = ctx->visualInfo;
    ng.ng_Flags = 0;
    ng.ng_GadgetID = 0;
    ng.ng_UserData = NULL;

    ng.ng_LeftEdge = UI_LABEL_X;
    ng.ng_TopEdge = cardRow;
    ng.ng_Width = UI_LABEL_WIDTH;
    ng.ng_Height = 12;
    ng.ng_GadgetText = NULL;
    last = ui_add_text_gadget(last, &ng, "Detected Card:", FALSE);

    ng.ng_LeftEdge = UI_VALUE_X;
    ng.ng_TopEdge = cardRow - UI_FIELD_Y_ADJUST;
    ng.ng_Width = UI_FULL_FIELD_WIDTH;
    ng.ng_Height = 14;
    ctx->cardValueGad = last = ui_add_text_gadget(last, &ng, ctx->cardText, TRUE);

    ng.ng_LeftEdge = UI_LABEL_X;
    ng.ng_TopEdge = installedRow;
    ng.ng_Width = UI_LABEL_WIDTH;
    ng.ng_Height = 12;
    last = ui_add_text_gadget(last, &ng, "Installed:", FALSE);

    ng.ng_LeftEdge = UI_VALUE_X;
    ng.ng_TopEdge = installedRow - UI_FIELD_Y_ADJUST;
    ng.ng_Width = UI_FULL_FIELD_WIDTH;
    ng.ng_Height = 14;
    ctx->flashValueGad = last = ui_add_text_gadget(last, &ng, ctx->flashText, TRUE);

    ng.ng_LeftEdge = UI_LABEL_X;
    ng.ng_TopEdge = bufferRow;
    ng.ng_Width = UI_LABEL_WIDTH;
    ng.ng_Height = 12;
    ng.ng_GadgetID = 0;
    ng.ng_GadgetText = NULL;
    last = ui_add_text_gadget(last, &ng, "Buffer:", FALSE);

    ng.ng_LeftEdge = UI_VALUE_X;
    ng.ng_TopEdge = bufferRow - UI_FIELD_Y_ADJUST;
    ng.ng_Width = UI_BUFFER_FIELD_WIDTH;
    ng.ng_Height = 14;
    ctx->bufferValueGad = last = ui_add_text_gadget(last, &ng, ctx->bufferText, TRUE);

    ng.ng_LeftEdge = UI_BUTTON_X;
    ng.ng_TopEdge = bufferRow - UI_FIELD_Y_ADJUST;
    ng.ng_Width = UI_SMALL_BUTTON_WIDTH;
    ng.ng_Height = 14;
    ng.ng_GadgetText = "Load ROM...";
    ng.ng_GadgetID = GID_LOAD_ROM;
    ctx->loadButtonGad = last = ui_add_button_gadget(last, &ng, FALSE);

    ng.ng_LeftEdge = UI_LABEL_X;
    ng.ng_TopEdge = progressRow;
    ng.ng_Width = UI_LABEL_WIDTH;
    ng.ng_Height = 12;
    ng.ng_GadgetID = 0;
    ng.ng_GadgetText = NULL;
    last = ui_add_text_gadget(last, &ng, "Progress:", FALSE);

    ng.ng_LeftEdge = UI_LABEL_X;
    ng.ng_TopEdge = statusRow;
    ng.ng_Width = UI_LABEL_WIDTH;
    ng.ng_Height = 12;
    ng.ng_GadgetID = 0;
    ng.ng_GadgetText = NULL;
    last = ui_add_text_gadget(last, &ng, "Status:", FALSE);

    ng.ng_LeftEdge = UI_VALUE_X;
    ng.ng_TopEdge = statusRow - UI_FIELD_Y_ADJUST;
    ng.ng_Width = UI_FULL_FIELD_WIDTH;
    ng.ng_Height = 14;
    ctx->statusValueGad = last = ui_add_text_gadget(last, &ng, ctx->statusText, TRUE);

    ng.ng_TopEdge = buttonRow;
    ng.ng_Height = 14;
    ng.ng_LeftEdge = UI_LABEL_X;
    ng.ng_Width = 100;
    ng.ng_GadgetText = "Erase Flash";
    ng.ng_GadgetID = GID_ERASE_FLASH;
    ctx->eraseButtonGad = last = ui_add_button_gadget(last, &ng,
                                                      !ctx->board.flashAvailable);

    ng.ng_LeftEdge = 118;
    ng.ng_Width = 86;
    ng.ng_GadgetText = "Program";
    ng.ng_GadgetID = GID_PROGRAM_FLASH;
    ctx->programButtonGad = last = ui_add_button_gadget(last, &ng, TRUE);

    ng.ng_LeftEdge = 214;
    ng.ng_Width = 150;
    ng.ng_GadgetText = "Manufacturing Info";
    ng.ng_GadgetID = GID_MANUFACTURING;
    ctx->mfgButtonGad = last = ui_add_button_gadget(last, &ng,
                                                    !ctx->board.flashAvailable);

    ng.ng_LeftEdge = 374;
    ng.ng_Width = 76;
    ng.ng_GadgetText = "About";
    ng.ng_GadgetID = GID_ABOUT;
    last = ui_add_button_gadget(last, &ng, FALSE);

    ng.ng_LeftEdge = 460;
    ng.ng_Width = 85;
    ng.ng_GadgetText = "Quit";
    ng.ng_GadgetID = GID_QUIT;
    last = ui_add_button_gadget(last, &ng, FALSE);

    return (last != NULL);
}

static void ui_drain_window_messages(struct Window *window)
{
    struct IntuiMessage *msg;

    if (!window || !window->UserPort)
        return;

    while ((msg = GT_GetIMsg(window->UserPort)) != NULL)
        GT_ReplyIMsg(msg);
}

static void ui_destroy(struct UiContext *ctx)
{
    flash_set_status_sink(NULL, NULL);

    ui_free_buffer(ctx);

    if (ctx->window) {
        ui_drain_window_messages(ctx->window);
        CloseWindow(ctx->window);
        ctx->window = NULL;
        ctx->gadgetsAdded = FALSE;
    }
    if (ctx->gadgets) {
        FreeGadgets(ctx->gadgets);
        ctx->gadgets = NULL;
    }
    if (ctx->visualInfo) {
        FreeVisualInfo(ctx->visualInfo);
        ctx->visualInfo = NULL;
    }
    if (ctx->screen && ctx->lockedPubScreen) {
        UnlockPubScreen(NULL, ctx->screen);
    }
    ctx->screen = NULL;
    ctx->lockedPubScreen = FALSE;
    if (AslBase) {
        CloseLibrary(AslBase);
        AslBase = NULL;
    }
    if (GadToolsBase) {
        CloseLibrary(GadToolsBase);
        GadToolsBase = NULL;
    }
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
    if (GfxBase) {
        CloseLibrary((struct Library *)GfxBase);
        GfxBase = NULL;
    }
    if (ExpansionBase) {
        CloseLibrary((struct Library *)ExpansionBase);
        ExpansionBase = NULL;
    }

    flash_cleanup();
}

static BOOL ui_open(struct UiContext *ctx, char *error, size_t error_len)
{
    struct TagItem window_tags[] = {
        { WA_Left, 20 },
        { WA_Top, 20 },
        { WA_Width, 560 },
        { WA_Height, UI_WINDOW_HEIGHT },
        { WA_Title, (ULONG)"A4092 Flash Utility" },
        { WA_CustomScreen, 0 },
        { WA_DragBar, TRUE },
        { WA_DepthGadget, TRUE },
        { WA_CloseGadget, TRUE },
        { WA_Activate, TRUE },
        { WA_SmartRefresh, TRUE },
        { WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_REFRESHWINDOW },
        { TAG_DONE, 0 }
    };
    char missing[128];

    memset(ctx, 0, sizeof(*ctx));
    strcpy(ctx->installedRom.summary, "No flash detected");
    strcpy(ctx->bufferedRom.summary, "Empty");
    strcpy(ctx->statusText, "Initializing GUI...");
    missing[0] = '\0';
    ui_set_open_error(error, error_len, "Unknown GUI initialization failure.");

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 36);
    if (!IntuitionBase)
        ui_append_missing_component(missing, sizeof(missing),
                                    "intuition.library v36");

    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 36);
    if (!GfxBase)
        ui_append_missing_component(missing, sizeof(missing),
                                    "graphics.library v36");

    GadToolsBase = OpenLibrary("gadtools.library", 36);
    if (!GadToolsBase)
        ui_append_missing_component(missing, sizeof(missing),
                                    "gadtools.library v36");

    if (missing[0]) {
        char missing_message[192];

        snprintf(missing_message, sizeof(missing_message),
                 "Missing required library%s: %s.",
                 strchr(missing, ',') ? "ies" : "", missing);
        ui_set_open_error(error, error_len, missing_message);
        return FALSE;
    }

    ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 0);
    if (!ExpansionBase)
        ui_append_missing_component(missing, sizeof(missing),
                                    "expansion.library");

    if (missing[0]) {
        char missing_message[192];

        snprintf(missing_message, sizeof(missing_message),
                 "Missing required library%s: %s.",
                 strchr(missing, ',') ? "ies" : "", missing);
        ui_set_open_error(error, error_len, missing_message);
        return FALSE;
    }

    if (IntuitionBase->ActiveWindow && IntuitionBase->ActiveWindow->WScreen) {
        ctx->screen = IntuitionBase->ActiveWindow->WScreen;
    } else if (IntuitionBase->ActiveScreen) {
        ctx->screen = IntuitionBase->ActiveScreen;
    } else if (IntuitionBase->FirstScreen) {
        ctx->screen = IntuitionBase->FirstScreen;
    } else {
        ctx->screen = LockPubScreen(NULL);
        if (ctx->screen)
            ctx->lockedPubScreen = TRUE;
    }

    if (!ctx->screen) {
        ui_set_open_error(error, error_len,
                          "No Intuition screen is available. Run LoadWB or open a public screen first.");
        return FALSE;
    }

    ctx->visualInfo = GetVisualInfoA(ctx->screen, NULL);
    if (!ctx->visualInfo) {
        ui_set_open_error(error, error_len,
                          "GetVisualInfoA() failed for the current screen.");
        return FALSE;
    }

    flash_set_status_sink(ui_status_sink, ctx);
    ui_scan_board(ctx);
    ui_refresh_state(ctx);

    window_tags[5].ti_Tag = ctx->lockedPubScreen ? WA_PubScreen : WA_CustomScreen;
    window_tags[5].ti_Data = (ULONG)ctx->screen;
    ctx->window = OpenWindowTagList(NULL, window_tags);
    if (!ctx->window) {
        ui_set_open_error(error, error_len,
                          "OpenWindowTagList() failed for the selected screen.");
        return FALSE;
    }

    if (!ui_build_gadgets(ctx)) {
        ui_set_open_error(error, error_len,
                          "Failed to create the GadTools gadget list.");
        return FALSE;
    }

    AddGList(ctx->window, ctx->gadgets, -1, -1, NULL);
    ctx->gadgetsAdded = TRUE;
    RefreshGList(ctx->gadgets, ctx->window, NULL, -1);
    GT_RefreshWindow(ctx->window, NULL);
    if (strcmp(ctx->statusText, "Initializing GUI...") == 0)
        ui_set_status(ctx, "Ready.");
    ui_refresh_state(ctx);
    return TRUE;
}

static BOOL ui_load_buffer_from_file(struct UiContext *ctx, const char *path)
{
    ULONG romSize;
    UBYTE *buffer;

    ui_set_status(ctx, "Loading ROM image...");
    if (ctx->window)
        GT_RefreshWindow(ctx->window, NULL);

    romSize = getFileSize((char *)path);
    if (romSize == 0) {
        ui_set_status(ctx, "Failed to load ROM image.");
        return FALSE;
    }

    if (romSize > 2048 * 1024 || romSize < 32 * 1024) {
        ui_set_status(ctx, "Failed to load ROM image.");
        return FALSE;
    }

    buffer = AllocMem(romSize, MEMF_ANY | MEMF_CLEAR);
    if (!buffer) {
        ui_set_status(ctx, "Failed to load ROM image.");
        return FALSE;
    }

    if (!readFileToBuf((char *)path, buffer)) {
        FreeMem(buffer, romSize);
        ui_set_status(ctx, "Failed to load ROM image.");
        return FALSE;
    }

    ui_free_buffer(ctx);
    ctx->buffer = buffer;
    ctx->bufferSize = romSize;
    strncpy(ctx->bufferPath, path, sizeof(ctx->bufferPath) - 1);
    ctx->bufferPath[sizeof(ctx->bufferPath) - 1] = '\0';
    summarize_rom_buffer(ctx->buffer, ctx->bufferSize, &ctx->bufferedRom);
    ui_set_status(ctx, "ROM image loaded into buffer.");
    ui_set_progress(ctx, 0);
    ui_refresh_state(ctx);
    return TRUE;
}

static BOOL ui_request_rom_path(struct UiContext *ctx, char *path,
                                size_t path_len)
{
    struct Window *window;
    struct Gadget *gadgets = NULL;
    struct Gadget *last = NULL;
    struct Gadget *pathGad = NULL;
    struct NewGadget ng;
    struct TagItem window_tags[] = {
        { WA_Left, 40 },
        { WA_Top, 40 },
        { WA_Width, 520 },
        { WA_Height, 68 },
        { WA_Title, (ULONG)"Load ROM image" },
        { WA_CustomScreen, 0 },
        { WA_DragBar, TRUE },
        { WA_DepthGadget, TRUE },
        { WA_CloseGadget, TRUE },
        { WA_Activate, TRUE },
        { WA_SmartRefresh, TRUE },
        { WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_REFRESHWINDOW },
        { TAG_DONE, 0 }
    };
    BOOL done = FALSE;
    BOOL accepted = FALSE;

    if (!ctx || !path || path_len == 0 || !ctx->screen || !ctx->visualInfo)
        return FALSE;

    window_tags[5].ti_Tag = ctx->lockedPubScreen ? WA_PubScreen : WA_CustomScreen;
    window_tags[5].ti_Data = (ULONG)ctx->screen;
    if (ctx->window) {
        window_tags[0].ti_Data = ctx->window->LeftEdge + 20;
        window_tags[1].ti_Data = ctx->window->TopEdge + 28;
    }

    window = OpenWindowTagList(NULL, window_tags);
    if (!window)
        return FALSE;

    gadgets = CreateContext(&last);
    if (!gadgets) {
        CloseWindow(window);
        return FALSE;
    }

    ng.ng_TextAttr = NULL;
    ng.ng_VisualInfo = ctx->visualInfo;
    ng.ng_Flags = 0;
    ng.ng_UserData = NULL;

    ng.ng_LeftEdge = 12;
    ng.ng_TopEdge = 22;
    ng.ng_Width = 72;
    ng.ng_Height = 12;
    ng.ng_GadgetText = NULL;
    ng.ng_GadgetID = 0;
    last = ui_add_text_gadget(last, &ng, "ROM path:", FALSE);

    ng.ng_LeftEdge = 92;
    ng.ng_TopEdge = 20;
    ng.ng_Width = 410;
    ng.ng_Height = 14;
    ng.ng_GadgetID = GID_ROM_PATH;
    pathGad = last = ui_add_string_gadget(last, &ng, path, (UWORD)path_len);

    ng.ng_TopEdge = 44;
    ng.ng_Height = 14;
    ng.ng_LeftEdge = 312;
    ng.ng_Width = 90;
    ng.ng_GadgetText = "Load ROM";
    ng.ng_GadgetID = GID_ROM_PATH_LOAD;
    last = ui_add_button_gadget(last, &ng, FALSE);

    ng.ng_LeftEdge = 414;
    ng.ng_Width = 88;
    ng.ng_GadgetText = "Cancel";
    ng.ng_GadgetID = GID_ROM_PATH_CANCEL;
    last = ui_add_button_gadget(last, &ng, FALSE);

    if (!last || !pathGad) {
        FreeGadgets(gadgets);
        CloseWindow(window);
        return FALSE;
    }

    AddGList(window, gadgets, -1, -1, NULL);
    RefreshGList(gadgets, window, NULL, -1);
    GT_RefreshWindow(window, NULL);
    if (pathGad)
        ActivateGadget(pathGad, window, NULL);

    while (!done) {
        struct IntuiMessage *msg;

        WaitPort(window->UserPort);
        while ((msg = GT_GetIMsg(window->UserPort)) != NULL) {
            ULONG msg_class = msg->Class;
            UWORD gadget_id = 0;

            if (msg_class == IDCMP_GADGETUP && msg->IAddress)
                gadget_id = ((struct Gadget *)msg->IAddress)->GadgetID;
            GT_ReplyIMsg(msg);

            switch (msg_class) {
                case IDCMP_CLOSEWINDOW:
                    done = TRUE;
                    break;

                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(window);
                    GT_EndRefresh(window, TRUE);
                    break;

                case IDCMP_GADGETUP:
                    if (gadget_id == GID_ROM_PATH_LOAD ||
                        gadget_id == GID_ROM_PATH) {
                        STRPTR value = NULL;
                        struct TagItem get_tags[] = {
                            { GTST_String, (ULONG)&value },
                            { TAG_DONE, 0 }
                        };

                        GT_GetGadgetAttrsA(pathGad, window, NULL, get_tags);
                        if (value && value[0]) {
                            strncpy(path, value, path_len - 1);
                            path[path_len - 1] = '\0';
                            accepted = TRUE;
                        }
                        done = TRUE;
                    } else if (gadget_id == GID_ROM_PATH_CANCEL) {
                        done = TRUE;
                    }
                    break;
            }
        }
    }

    CloseWindow(window);
    FreeGadgets(gadgets);
    return accepted;
}

static void ui_handle_load_rom(struct UiContext *ctx)
{
    struct FileRequester *fr;
    struct TagItem fr_tags[] = {
        { ASLFR_Window, 0 },
        { ASLFR_TitleText, (ULONG)"Load ROM image" },
        { ASLFR_InitialDrawer, 0 },
        { ASLFR_InitialFile, 0 },
        { TAG_DONE, 0 }
    };
    char drawer[128];
    char file[64];
    char path[256];
    BOOL opened_asl_here = FALSE;

    ui_set_default_rom_target(ctx->board.type, drawer, sizeof(drawer), file,
                              sizeof(file));
    ui_set_default_rom_path(ctx->board.type, path, sizeof(path));
    if (!AslBase) {
        AslBase = OpenLibrary("asl.library", 36);
        if (!AslBase) {
            if (ui_request_rom_path(ctx, path, sizeof(path)) &&
                !ui_load_buffer_from_file(ctx, path)) {
                ui_request(ctx->window, "Load ROM",
                           "Failed to load the selected ROM image.\n"
                           "Expected a ROM file between 32KB and 2MB.",
                           "OK");
            }
            return;
        }
        opened_asl_here = TRUE;
    }

    fr = AllocAslRequest(ASL_FileRequest, NULL);
    if (!fr) {
        if (opened_asl_here) {
            CloseLibrary(AslBase);
            AslBase = NULL;
        }
        if (ui_request_rom_path(ctx, path, sizeof(path)) &&
            !ui_load_buffer_from_file(ctx, path)) {
            ui_request(ctx->window, "Load ROM",
                       "Failed to load the selected ROM image.\n"
                       "Expected a ROM file between 32KB and 2MB.",
                       "OK");
        }
        return;
    }

    fr_tags[0].ti_Data = (ULONG)ctx->window;
    fr_tags[2].ti_Data = (ULONG)drawer;
    fr_tags[3].ti_Data = (ULONG)file;

    if (AslRequest(fr, fr_tags)) {
        strncpy(path, fr->fr_Drawer, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        AddPart(path, fr->fr_File, sizeof(path));

        if (!ui_load_buffer_from_file(ctx, path)) {
            ui_request(ctx->window, "Load ROM",
                       "Failed to load the selected ROM image.\n"
                       "Expected a ROM file between 32KB and 2MB.",
                       "OK");
        }
    }

    FreeAslRequest(fr);
    if (opened_asl_here) {
        CloseLibrary(AslBase);
        AslBase = NULL;
    }
}

static void ui_handle_erase_flash(struct UiContext *ctx)
{
    ULONG eraseSize;

    if (!ctx->board.flashAvailable) {
        ui_request(ctx->window, "Erase Flash",
                   "No programmable flash was detected on this card.", "OK");
        return;
    }

    eraseSize = ui_rom_region_size(ctx);
    if (eraseSize == 0 || ctx->board.sectorSize == 0) {
        ui_request(ctx->window, "Erase Flash",
                   "Unknown erase geometry for this flash part.", "OK");
        return;
    }

    if (ui_request(ctx->window, "Erase Flash",
                   "Erase the ROM region only?\n"
                   "OEM, NVRAM, and manufacturing data will be preserved.",
                   "Erase|Cancel") != 1) {
        return;
    }

    if (!ui_begin_flash_access()) {
        ui_request(ctx->window, "Erase Flash",
                   "Failed to inhibit AmigaDOS devices.\n"
                   "Wait for disk activity to stop and try again.",
                   "OK");
        return;
    }

    ui_set_status(ctx, "Erasing ROM region...");
    ui_set_progress(ctx, 0);

    if (!ui_erase_region(ctx, 0, eraseSize, 0, 100)) {
        ui_end_flash_access();
        ui_set_status(ctx, "Flash erase failed.");
        ui_request(ctx->window, "Erase Flash", "Flash erase failed.", "OK");
        return;
    }

    ui_end_flash_access();
    inspect_flash_contents(ctx->board.flashSize, &ctx->installedRom);
    ui_set_status(ctx, "ROM region erase completed.");
    ui_refresh_state(ctx);
    ui_request(ctx->window, "Erase Flash",
               "ROM region erase completed.\n"
               "OEM, NVRAM, and manufacturing data were preserved.",
               "OK");
}

static void ui_handle_program_flash(struct UiContext *ctx)
{
    ULONG eraseSize;
    ULONG romRegionSize;

    if (!ctx->board.flashAvailable) {
        ui_request(ctx->window, "Program Flash",
                   "No programmable flash was detected on this card.", "OK");
        return;
    }

    if (!ctx->buffer || ctx->bufferSize == 0) {
        ui_request(ctx->window, "Program Flash",
                   "Load a ROM image into the buffer first.", "OK");
        return;
    }

    romRegionSize = ui_rom_region_size(ctx);
    if (ctx->bufferSize > romRegionSize) {
        ui_request(ctx->window, "Program Flash",
                   "The loaded ROM image is larger than the programmable ROM region.",
                   "OK");
        return;
    }

    if (ui_request(ctx->window, "Program Flash",
                   "Program the loaded ROM image into flash?\n"
                   "This updates the ROM region and preserves OEM, NVRAM,\n"
                   "and manufacturing data.",
                   "Program|Cancel") != 1) {
        return;
    }

    if (!ui_begin_flash_access()) {
        ui_request(ctx->window, "Program Flash",
                   "Failed to inhibit AmigaDOS devices.\n"
                   "Wait for disk activity to stop and try again.",
                   "OK");
        return;
    }

    eraseSize = ui_program_erase_size(ctx);
    if (eraseSize == 0 || ctx->board.sectorSize == 0) {
        ui_end_flash_access();
        ui_request(ctx->window, "Program Flash",
                   "Unknown erase geometry for this flash part.", "OK");
        return;
    }

    ui_set_status(ctx, "Erasing ROM region...");
    ui_set_progress(ctx, 0);

    if (!ui_erase_region(ctx, 0, eraseSize, 0, 25)) {
        ui_end_flash_access();
        ui_set_status(ctx, "Failed to erase ROM region.");
        ui_request(ctx->window, "Program Flash",
                   "Failed to erase the ROM region before programming.", "OK");
        return;
    }

    if (!writeBufToFlashWithProgress(&ctx->board.board, ctx->buffer,
                                     ctx->board.board.flashbase,
                                     ctx->bufferSize, ui_program_write_progress,
                                     ctx)) {
        ui_end_flash_access();
        ui_set_status(ctx, "Flash programming failed.");
        ui_request(ctx->window, "Program Flash", "Flash programming failed.",
                   "OK");
        return;
    }

    ui_end_flash_access();
    inspect_flash_contents(ctx->board.flashSize, &ctx->installedRom);
    ui_set_status(ctx, "Flash programming completed.");
    ui_refresh_state(ctx);
    ui_request(ctx->window, "Program Flash", "Flash programming completed.",
               "OK");
}

static void ui_handle_manufacturing_info(struct UiContext *ctx)
{
    struct mfg_data mfg;
    BOOL checksum_ok = FALSE;
    char text[768];

    if (!ctx->board.flashAvailable) {
        ui_request(ctx->window, "Manufacturing Info",
                   "No flash is available on this card.", "OK");
        return;
    }

    if (!read_mfg_data(&mfg, &checksum_ok)) {
        ui_request(ctx->window, "Manufacturing Info",
                   "No manufacturing data found.", "OK");
        return;
    }

    ui_format_mfg_summary(text, sizeof(text), &mfg, checksum_ok);
    ui_request(ctx->window, "Manufacturing Info", text, "OK");
}

static void ui_handle_about(struct UiContext *ctx)
{
    char text[256];

    snprintf(text, sizeof(text),
             "%s\n\n"
             "Workbench flash utility for A4092 and A4770 controllers.\n"
             "(C) 2026 Stefan Reinauer",
             VERSION);
    ui_request(ctx->window, "About a4092flash", text, "OK");
}

int run_workbench_ui(void)
{
    struct UiContext ctx;
    char error[192];
    BOOL running = TRUE;
    memset(&ctx, 0, sizeof(ctx));

    if (!ui_open(&ctx, error, sizeof(error))) {
        /* Release any libraries or UI state opened before init failed. */
        ui_destroy(&ctx);
        return 20;
    }

    while (running) {
        struct IntuiMessage *msg;

        WaitPort(ctx.window->UserPort);
        while ((msg = GT_GetIMsg(ctx.window->UserPort)) != NULL) {
            ULONG msg_class = msg->Class;
            UWORD gadget_id = 0;

            if (msg_class == IDCMP_GADGETUP && msg->IAddress)
                gadget_id = ((struct Gadget *)msg->IAddress)->GadgetID;
            GT_ReplyIMsg(msg);

            switch (msg_class) {
                case IDCMP_CLOSEWINDOW:
                    running = FALSE;
                    break;

                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(ctx.window);
                    GT_EndRefresh(ctx.window, TRUE);
                    ui_refresh_state(&ctx);
                    break;

                case IDCMP_GADGETUP:
                    switch (gadget_id) {
                        case GID_LOAD_ROM:
                            ui_handle_load_rom(&ctx);
                            break;
                        case GID_ERASE_FLASH:
                            ui_handle_erase_flash(&ctx);
                            break;
                        case GID_PROGRAM_FLASH:
                            ui_handle_program_flash(&ctx);
                            break;
                        case GID_MANUFACTURING:
                            ui_handle_manufacturing_info(&ctx);
                            break;
                        case GID_ABOUT:
                            ui_handle_about(&ctx);
                            break;
                        case GID_QUIT:
                            running = FALSE;
                            break;
                    }
                    break;
            }
        }
    }

    ui_destroy(&ctx);
    return 0;
}
