// SPDX-License-Identifier: GPL-2.0-only

#include <string.h>
#include <stdio.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <utility/tagitem.h>
#include <graphics/displayinfo.h>

#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>

#include "flash.h"
#include "gui.h"
#include "hardware.h"
#include "main.h"

struct IntuitionBase *IntuitionBase = NULL;
struct Library *GadToolsBase = NULL;
struct Library *AslBase = NULL;

struct UiContext {
  struct Window *window;
  struct Screen *screen;
  APTR visualInfo;
  struct Gadget *gadgets;
  struct Gadget *cardValueGad;
  struct Gadget *flashValueGad;
  struct Gadget *bufferValueGad;
  struct Gadget *loadButtonGad;
  struct Gadget *eraseButtonGad;
  struct Gadget *programButtonGad;
  struct Gadget *mfgButtonGad;
  UBYTE *buffer;
  ULONG bufferSize;
  char bufferPath[256];
  char cardText[160];
  char flashText[160];
  char bufferText[160];
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
  GID_QUIT
};

extern struct ExpansionBase *ExpansionBase;

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

static void ui_set_default_rom_target(enum boardType type, char *drawer,
                                      size_t drawer_len, char *file,
                                      size_t file_len)
{
    strncpy(drawer, "PROGDIR:ROMs", drawer_len - 1);
    drawer[drawer_len - 1] = '\0';

    switch (type) {
        case BOARD_A4770:
            strncpy(file, "a4770_cdfs.rom", file_len - 1);
            break;
        case BOARD_A4092:
            strncpy(file, "a4092_cdfs.rom", file_len - 1);
            break;
        default:
            file[0] = '\0';
            return;
    }

    file[file_len - 1] = '\0';
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

static void ui_format_mfg_summary(char *buf, size_t buf_len,
                                  const struct mfg_data *mfg, BOOL checksum_ok)
{
    char card_type[sizeof(mfg->card_type) + 1];
    char serial[sizeof(mfg->serial) + 1];
    char sku[sizeof(mfg->sku) + 1];
    char owner[sizeof(mfg->owner_name) + 1];

    ui_copy_mfg_field(card_type, sizeof(card_type), mfg->card_type,
                      sizeof(mfg->card_type));
    ui_copy_mfg_field(serial, sizeof(serial), mfg->serial, sizeof(mfg->serial));
    ui_copy_mfg_field(sku, sizeof(sku), mfg->sku, sizeof(mfg->sku));
    ui_copy_mfg_field(owner, sizeof(owner), mfg->owner_name,
                      sizeof(mfg->owner_name));

    snprintf(buf, buf_len,
             "Card: %s\n"
             "Serial: %s\n"
             "SKU: %s\n"
             "HW Rev: %u.%u\n"
             "CPLD: %u.%u\n"
             "Initial FW: %u.%u\n"
             "Owner: %s\n"
             "Test Status: 0x%04X\n"
             "Checksum: %s",
             card_type[0] ? card_type : "-",
             serial[0] ? serial : "-",
             sku[0] ? sku : "-",
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

    memset(&ctx->board, 0, sizeof(ctx->board));
    memset(&ctx->installedRom, 0, sizeof(ctx->installedRom));
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
}

static void ui_refresh_state(struct UiContext *ctx)
{
    BOOL can_flash = ctx->board.flashAvailable;
    BOOL can_program = can_flash && ctx->buffer && ctx->bufferSize > 0;
    BOOL can_mfg = can_flash;

    snprintf(ctx->cardText, sizeof(ctx->cardText), "Detected card: %s",
             board_type_name(ctx->board.present ? ctx->board.type : BOARD_NONE));
    snprintf(ctx->flashText, sizeof(ctx->flashText), "Installed ROM: %.144s",
             ctx->installedRom.summary[0] ? ctx->installedRom.summary : "Unknown");
    snprintf(ctx->bufferText, sizeof(ctx->bufferText), "Buffer: %.151s",
             ctx->buffer && ctx->bufferSize ? ctx->bufferedRom.summary : "Empty");

    if (!ctx->window)
        return;

    ui_set_text_gadget(ctx->cardValueGad, ctx->window, ctx->cardText);
    ui_set_text_gadget(ctx->flashValueGad, ctx->window, ctx->flashText);
    ui_set_text_gadget(ctx->bufferValueGad, ctx->window, ctx->bufferText);
    ui_set_disabled_gadget(ctx->eraseButtonGad, ctx->window, !can_flash);
    ui_set_disabled_gadget(ctx->programButtonGad, ctx->window, !can_program);
    ui_set_disabled_gadget(ctx->mfgButtonGad, ctx->window, !can_mfg);
    GT_RefreshWindow(ctx->window, NULL);
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

static BOOL ui_build_gadgets(struct UiContext *ctx)
{
    struct Gadget *last = NULL;
    struct NewGadget ng;

    ctx->gadgets = CreateContext(&last);
    if (!ctx->gadgets)
        return FALSE;

    ng.ng_TextAttr = NULL;
    ng.ng_VisualInfo = ctx->visualInfo;
    ng.ng_Flags = 0;
    ng.ng_GadgetID = 0;
    ng.ng_UserData = NULL;

    ng.ng_LeftEdge = 10;
    ng.ng_TopEdge = 12;
    ng.ng_Width = 100;
    ng.ng_Height = 12;
    ng.ng_GadgetText = NULL;
    last = ui_add_text_gadget(last, &ng, "Card:", FALSE);

    ng.ng_LeftEdge = 120;
    ng.ng_TopEdge = 10;
    ng.ng_Width = 425;
    ng.ng_Height = 14;
    ctx->cardValueGad = last = ui_add_text_gadget(last, &ng, ctx->cardText, TRUE);

    ng.ng_LeftEdge = 10;
    ng.ng_TopEdge = 32;
    ng.ng_Width = 100;
    ng.ng_Height = 12;
    last = ui_add_text_gadget(last, &ng, "Installed:", FALSE);

    ng.ng_LeftEdge = 120;
    ng.ng_TopEdge = 30;
    ng.ng_Width = 315;
    ng.ng_Height = 14;
    ctx->flashValueGad = last = ui_add_text_gadget(last, &ng, ctx->flashText, TRUE);

    ng.ng_LeftEdge = 445;
    ng.ng_TopEdge = 30;
    ng.ng_Width = 100;
    ng.ng_Height = 14;
    ng.ng_GadgetText = "Load ROM...";
    ng.ng_GadgetID = GID_LOAD_ROM;
    ctx->loadButtonGad = last = ui_add_button_gadget(last, &ng, FALSE);

    ng.ng_LeftEdge = 10;
    ng.ng_TopEdge = 52;
    ng.ng_Width = 100;
    ng.ng_Height = 12;
    ng.ng_GadgetID = 0;
    ng.ng_GadgetText = NULL;
    last = ui_add_text_gadget(last, &ng, "Buffer:", FALSE);

    ng.ng_LeftEdge = 120;
    ng.ng_TopEdge = 50;
    ng.ng_Width = 425;
    ng.ng_Height = 14;
    ctx->bufferValueGad = last = ui_add_text_gadget(last, &ng, ctx->bufferText, TRUE);

    ng.ng_TopEdge = 78;
    ng.ng_Height = 14;

    ng.ng_LeftEdge = 10;
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

static void ui_destroy(struct UiContext *ctx)
{
    ui_free_buffer(ctx);

    if (ctx->window) {
        CloseWindow(ctx->window);
        ctx->window = NULL;
    }
    if (ctx->gadgets) {
        FreeGadgets(ctx->gadgets);
        ctx->gadgets = NULL;
    }
    if (ctx->visualInfo) {
        FreeVisualInfo(ctx->visualInfo);
        ctx->visualInfo = NULL;
    }
    if (ctx->screen) {
        UnlockPubScreen(NULL, ctx->screen);
        ctx->screen = NULL;
    }
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
    if (ExpansionBase) {
        CloseLibrary((struct Library *)ExpansionBase);
        ExpansionBase = NULL;
    }
    flash_cleanup();
}

static BOOL ui_open(struct UiContext *ctx)
{
    struct TagItem window_tags[] = {
        { WA_Left, 20 },
        { WA_Top, 20 },
        { WA_Width, 560 },
        { WA_Height, 118 },
        { WA_Title, (ULONG)"A4092 Flash Utility" },
        { WA_PubScreen, 0 },
        { WA_DragBar, TRUE },
        { WA_DepthGadget, TRUE },
        { WA_CloseGadget, TRUE },
        { WA_Activate, TRUE },
        { WA_SmartRefresh, TRUE },
        { WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_REFRESHWINDOW },
        { TAG_DONE, 0 }
    };

    memset(ctx, 0, sizeof(*ctx));
    strcpy(ctx->installedRom.summary, "No flash detected");
    strcpy(ctx->bufferedRom.summary, "Empty");

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 36);
    GadToolsBase = OpenLibrary("gadtools.library", 36);
    AslBase = OpenLibrary("asl.library", 36);
    ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 0);

    if (!IntuitionBase || !GadToolsBase || !AslBase || !ExpansionBase)
        return FALSE;

    ctx->screen = LockPubScreen(NULL);
    if (!ctx->screen)
        return FALSE;

    ctx->visualInfo = GetVisualInfoA(ctx->screen, NULL);
    if (!ctx->visualInfo)
        return FALSE;

    ui_scan_board(ctx);
    ui_refresh_state(ctx);

    window_tags[5].ti_Data = (ULONG)ctx->screen;
    ctx->window = OpenWindowTagList(NULL, window_tags);
    if (!ctx->window)
        return FALSE;

    if (!ui_build_gadgets(ctx))
        return FALSE;

    AddGList(ctx->window, ctx->gadgets, -1, -1, NULL);
    RefreshGList(ctx->gadgets, ctx->window, NULL, -1);
    GT_RefreshWindow(ctx->window, NULL);
    ui_refresh_state(ctx);
    return TRUE;
}

static BOOL ui_load_buffer_from_file(struct UiContext *ctx, const char *path)
{
    ULONG romSize;
    UBYTE *buffer;

    romSize = getFileSize((char *)path);
    if (romSize == 0)
        return FALSE;

    if (romSize > 2048 * 1024 || romSize < 32 * 1024)
        return FALSE;

    buffer = AllocMem(romSize, MEMF_ANY | MEMF_CLEAR);
    if (!buffer)
        return FALSE;

    if (!readFileToBuf((char *)path, buffer)) {
        FreeMem(buffer, romSize);
        return FALSE;
    }

    ui_free_buffer(ctx);
    ctx->buffer = buffer;
    ctx->bufferSize = romSize;
    strncpy(ctx->bufferPath, path, sizeof(ctx->bufferPath) - 1);
    ctx->bufferPath[sizeof(ctx->bufferPath) - 1] = '\0';
    summarize_rom_buffer(ctx->buffer, ctx->bufferSize, &ctx->bufferedRom);
    ui_refresh_state(ctx);
    return TRUE;
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

    ui_set_default_rom_target(ctx->board.type, drawer, sizeof(drawer), file,
                              sizeof(file));
    fr = AllocAslRequest(ASL_FileRequest, NULL);
    if (!fr) {
        ui_request(ctx->window, "Load ROM", "Could not allocate file requester.",
                   "OK");
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
}

static void ui_handle_erase_flash(struct UiContext *ctx)
{
    if (!ctx->board.flashAvailable) {
        ui_request(ctx->window, "Erase Flash",
                   "No programmable flash was detected on this card.", "OK");
        return;
    }

    if (ui_request(ctx->window, "Erase Flash",
                   "Erase the entire flash chip?\n"
                   "This also removes OEM, NVRAM, and manufacturing data.",
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

    if (!flash_erase_chip()) {
        ui_end_flash_access();
        ui_request(ctx->window, "Erase Flash", "Flash erase failed.", "OK");
        return;
    }

    ui_end_flash_access();
    inspect_flash_contents(ctx->board.flashSize, &ctx->installedRom);
    ui_refresh_state(ctx);
    ui_request(ctx->window, "Erase Flash", "Flash erase completed.", "OK");
}

static void ui_handle_program_flash(struct UiContext *ctx)
{
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

    if (ui_request(ctx->window, "Program Flash",
                   "Program the loaded ROM image into flash?\n"
                   "This updates the ROM region but keeps the rest of flash intact.",
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

    if (ctx->board.sectorSize > 0) {
        if (!flash_erase_bank(0, ctx->board.sectorSize, 65536)) {
            ui_end_flash_access();
            ui_request(ctx->window, "Program Flash",
                       "Failed to erase the ROM bank before programming.", "OK");
            return;
        }
    } else {
        ui_end_flash_access();
        ui_request(ctx->window, "Program Flash",
                   "Unknown erase geometry for this flash part.", "OK");
        return;
    }

    if (!writeBufToFlash(&ctx->board.board, ctx->buffer,
                         ctx->board.board.flashbase, ctx->bufferSize)) {
        ui_end_flash_access();
        ui_request(ctx->window, "Program Flash", "Flash programming failed.",
                   "OK");
        return;
    }

    ui_end_flash_access();
    inspect_flash_contents(ctx->board.flashSize, &ctx->installedRom);
    ui_refresh_state(ctx);
    ui_request(ctx->window, "Program Flash", "Flash programming completed.",
               "OK");
}

static void ui_handle_manufacturing_info(struct UiContext *ctx)
{
    struct mfg_data mfg;
    BOOL checksum_ok = FALSE;
    char text[512];

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
             "Shell mode remains available for scripting and manufacturing use.",
             VERSION);
    ui_request(ctx->window, "About a4092flash", text, "OK");
}

int run_workbench_ui(void)
{
    struct UiContext ctx;
    BOOL running = TRUE;

    if (!ui_open(&ctx)) {
        if (IntuitionBase)
            ui_request(NULL, "A4092 Flash Utility",
                       "Failed to open the Workbench user interface.\n"
                       "Kickstart/Workbench 2.0 or newer is required.",
                       "Quit");
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
