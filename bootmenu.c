#ifdef DEBUG_BOOTMENU
#define USE_SERIAL_OUTPUT
#endif

#include "port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/execbase.h>
#include <exec/types.h>
#include <exec/libraries.h>

#include <proto/exec.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>

#include <graphics/text.h>
#include <graphics/displayinfo.h>
#include <graphics/videocontrol.h>
#include <graphics/rastport.h>
#include <graphics/gfxmacros.h>

#include <graphics/gfxbase.h>
#include <clib/intuition_protos.h>
#include <clib/graphics_protos.h>
#include <clib/gadtools_protos.h>
#include "bootmenu.h"
#include "device.h"
#include "scsipiconf.h"
#include "siopreg.h"
#include "siopvar.h"
#include "attach.h"
#include "scsimsg.h"

extern struct ExecBase *SysBase;
struct GfxBase *GfxBase;
struct Library *GadToolsBase;
struct Library *IntuitionBase;
struct Screen *screen;
struct Window *window;
APTR visualInfo;
struct Gadget *gadgets;
struct Gadget *DisplayTypeGad;
struct Gadget *LastAdded;
struct NewGadget *NewGadget;

#define DISKS_BACK_ID      1
#define DIPSWITCH_BACK_ID  2
#define ABOUT_BACK_ID      3
#define DEBUG_BACK_ID      4
#define MAIN_ABOUT_ID      5
#define MAIN_DISKS_ID      6
#define MAIN_DIPSWITCH_ID  7
#define MAIN_DEBUG_ID      8
#define MAIN_BOOT_ID       9

#define ARRAY_LENGTH(array) (sizeof((array))/sizeof((array)[0]))
#define WIDTH  640

static const struct TextAttr font_attr =
{
    "topaz.font",
     8,
     FS_NORMAL,
     FPF_ROMFONT
};

static void init_bootmenu(void)
{
    UWORD pens = 0xffff;
    struct TagItem taglist[2];

    taglist[0].ti_Tag  = VTAG_BORDERSPRITE_SET;
    taglist[0].ti_Data = TRUE;
    taglist[1].ti_Tag  = TAG_DONE;

    screen = OpenScreenTags(NULL,
	SA_Depth,        2,
        SA_Font,         &font_attr,
        SA_Type,         CUSTOMSCREEN,
        SA_DisplayID,    PAL_MONITOR_ID|HIRES_KEY,
        SA_Interleaved,  TRUE,
        SA_Draggable,    FALSE,
        SA_Quiet,        TRUE,
        SA_Pens,         &pens,
        SA_VideoControl, taglist,
        TAG_DONE);

    window = OpenWindowTags(NULL,
	WA_IDCMP,         (IDCMP_RAWKEY | BUTTONIDCMP | LISTVIEWIDCMP | MXIDCMP),
        WA_CustomScreen,  screen,
        WA_Flags,         (WFLG_NOCAREREFRESH | WFLG_BORDERLESS | WFLG_ACTIVATE | WFLG_RMBTRAP),
        TAG_DONE);

    visualInfo = GetVisualInfoA(screen,NULL);
}

static void cleanup_bootmenu(void)
{
    CloseWindow(window);
    FreeVisualInfo(visualInfo);
    CloseScreen(screen);
    FreeGadgets(gadgets);

    CloseLibrary(IntuitionBase);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary(GadToolsBase);
}

static struct Gadget *create_gadget_custom(UWORD kind, ULONG tag1, ...)
{
    return(CreateGadgetA(kind,LastAdded,NewGadget,(struct TagItem *)&tag1));
}

static struct Gadget *create_gadget(UWORD kind)
{
    return(create_gadget_custom(kind,TAG_DONE));
}

static void Print(STRPTR text, UWORD x, UWORD y, int center)
{
    struct RastPort *rp = &screen->RastPort;
    if (center)
        x += (WIDTH-TextLength(rp,text,strlen(text)))/2;
    Move(rp,x,y);
    Text(rp,text,strlen(text));
}

static void page_header(struct NewGadget *ng, STRPTR title)
{
    struct RastPort *rp = &screen->RastPort;
    if (gadgets)
    {
        RemoveGList(window,gadgets,-1);
        FreeGadgets(gadgets);
        DisplayTypeGad = NULL;
        LastAdded = NULL;
        SetRast(&screen->RastPort,0);
    }
    SetAPen(rp,2);
    Print(title,0,13,TRUE);

    gadgets = CreateContext(&LastAdded);

    NewGadget  = ng;
    ng->ng_TextAttr   = NULL;
    ng->ng_Flags      = 0;
    ng->ng_VisualInfo = visualInfo;
    ng->ng_Width      = 87;
    ng->ng_Height     = 14;
    ng->ng_TopEdge    = 183;

    SetAPen(rp,1);
    Print("Welcome to your Amiga 4091 Zorro-III SCSI-II Host Controller",0,33,TRUE);
    Print("This project is brought to you by Chris Hooper and Stefan Reinauer",0,41,TRUE);
}

static void page_footer(void)
{
    AddGList(window,gadgets,-1,-1,NULL);
    RefreshGList(gadgets,window,NULL,-1);
    GT_RefreshWindow(window,NULL);
}

static void draw_dipswitch(UWORD x, UWORD y, UWORD on)
{
    struct RastPort *rp = &screen->RastPort;

    SetAPen(rp, 2);
    SetOPen(rp, 1);
    RectFill(rp, x+10, y, x+52,y+8);
    if (on)
	x+= 20;
    SetAPen(rp, 0);
    RectFill(rp, x+12, y+2, x+30,y+6);
}

static volatile char *dipswitch_text(int val, int num)
{
    static volatile char string[24];
    string[0]=0;
    switch (num) {
      case 8: strcat((char *)string, "SCSI LUNs ");
	      strcat((char *)string, val?"Enabled":"Disabled");
	      break;
      case 7: strcat((char *)string, "External Termination ");
	      strcat((char *)string, val?"On":"Off");
	      break;
      case 6: strcat((char *)string, val?"As":"S");
	      strcat((char *)string, "ynchronous SCSI Mode");
	      break;
      case 5: strcat((char *)string, val?"Long":"Short");
	      strcat((char *)string, " Spinup Mode");
	      break;
      case 4: strcat((char *)string, val?"SCSI-1 Slow":"SCSI-2 Fast");
	      strcat((char *)string, " Bus Mode");
	      break;
      case 3: strcat((char *)string, "SCSI Address A");
	      strcat((char *)string, val?"2 = 0":"2 = 1");
	      break;
      case 2: strcat((char *)string, "SCSI Address A");
	      strcat((char *)string, val?"1 = 0":"1 = 1");
	      break;
      case 1: strcat((char *)string, "SCSI Address A");
	      strcat((char *)string, val?"0 = 0":"0 = 1");
	      break;
    }

    return  string;
}

extern a4091_save_t *asave;
static void draw_dipswitches(UWORD x, UWORD y)
{
    struct RastPort *rp = &screen->RastPort;
    int i;
    UBYTE dip_switches;
    volatile char *ret, *num="8", *hostid="Host ID: 7";
    num[0]='8';

    if (asave) {
        dip_switches = *(uint8_t *)((asave->as_addr) + 0x008c0003);
	printf("addr=%x\n",asave->as_addr);
	printf("dip_switches=%x\n",dip_switches);
	dip_switches=~dip_switches;
    } else {
	dip_switches = 0x00;
    }

    SetAPen(rp, 1);
    SetBPen(rp, 0);
    Move(rp, x+16, y);
    Text(rp, "Off/On",6);

    SetAPen(rp, 3);    /* Set A pen color. */
    SetOPen(rp, 1);
    RectFill(rp, x, y+2, x+70, y+90);

    for (i=0; i<8; i++) {
        draw_dipswitch(x+8, y+7+(i*10), (dip_switches&(1<<(7-i))));
    }

    for (i=0; i<8; i++) {
        SetAPen(rp, 1);
        SetBPen(rp, 3);
	Move(rp, x+6,y+(i*10)+14);
        Text(rp, (char *)num, 1);
	num[0]--;
	Move(rp, x+82,y+(i*10)+14);
        SetBPen(rp, 0);
	ret = dipswitch_text((dip_switches&(1<<(7-i))), 8-i);
        Text(rp, (char *)ret, strlen((char *)ret));
    }

    hostid[9]='0'+((~dip_switches)&0x7);
    SetAPen(rp, 1);
    SetBPen(rp, 0);
    Move(rp, x+280, y+64);
    Text(rp, (char *)hostid,10);
    BNDRYOFF(rp);
}

static void dipswitch_page(void)
{
    struct NewGadget ng;
    page_header(&ng, "A4091 Diagnostics - DIP switches");

    SetRGB4(&screen->ViewPort,3,11,8,8);

    ng.ng_LeftEdge   = 400;
    ng.ng_TopEdge    = 145;
    ng.ng_Width      = 120;
    ng.ng_GadgetText = "Back";
    ng.ng_GadgetID   = DIPSWITCH_BACK_ID;
    LastAdded = create_gadget(BUTTON_KIND);

    DrawBevelBox(&screen->RastPort,100,52,440,115,
                                    GT_VisualInfo,  visualInfo,
                                    GTBB_Recessed,  TRUE,
                                    GTBB_FrameType, BBFT_RIDGE,
                                    TAG_DONE);
    draw_dipswitches(120,65);

    page_footer();
}

extern const char device_id_string[];
static void about_page(void)
{
    struct NewGadget ng;
    page_header(&ng, "About A4091");
    SetAPen(&screen->RastPort, 1);
    Print("Thank you to Dave Haynie, Scott Schaeffer, Greg", 118,68,FALSE);
    Print("Berlin and Terry Fisher for the A4091",118,76,FALSE);
    Print("Greetings to the Amiga community.",118,92,FALSE);
    Print("Only Amiga makes it possible.", 192,120,FALSE);
    Print((char *)device_id_string, 118,155,FALSE);

    ng.ng_LeftEdge   = 400;
    ng.ng_TopEdge    = 145;
    ng.ng_Width      = 120;
    ng.ng_GadgetText = "Back";
    ng.ng_GadgetID   = ABOUT_BACK_ID;
    LastAdded = create_gadget(BUTTON_KIND);

    DrawBevelBox(&screen->RastPort,100,52,440,115,
                                    GT_VisualInfo,  visualInfo,
                                    GTBB_Recessed,  TRUE,
                                    GTBB_FrameType, BBFT_RIDGE,
                                    TAG_DONE);
    page_footer();
}

#ifdef DEBUG_BOOTMENU
static char *
trim_spaces(char *str, size_t len)
{
    size_t olen = len;
    char *ptr;

    for (ptr = str; len > 0; ptr++, len--)
        if (*ptr != ' ')
            break;

    if (len == 0) {
        /* Completely empty name */
        *str = '\0';
        return (str);
    } else {
        memmove(str, ptr, len);

        while ((len > 0) && (str[len - 1] == ' '))
            len--;

        if (len < olen)  /* Is there space for a NIL character? */
            str[len] = '\0';
        return (str);
    }
    return (str);
}
#endif
static const char *
devtype_str(uint dtype)
{
    static const char * const dt_list[] = {
        "Disk", "Tape", "Printer", "Proc",
        "Worm", "CDROM", "Scanner", "Optical",
        "Changer", "Comm", "ASCIT81", "ASCIT82",
    };
    if (dtype < ARRAY_SIZE(dt_list))
        return (dt_list[dtype]);
    return ("Unknown");
}

static char _itoabuf[12]; // MAXINT
int scan_disks(void)
{
    int i;
    int cnt=0;
    int erc;
    struct IOExtTD tio;
    int unit=0;
    struct RastPort *rp = &screen->RastPort;

    int x,y;
    printf("Looking for disks!\n");

    for (i=0; i<7; i++) { // FIXME LUNs?
	//unit=i;
        x=52;
	y=82+(cnt*10);
	if (safe_open((struct IOStdReq *)&tio, i))
	    continue;

	scsi_inquiry_data_t *inq_res;
	erc = do_scsi_inquiry(&tio, unit, &inq_res);
	if (erc == 0) {
	    char unit_str[]="0.0";
	    unit_str[0]='0'+i;
            Move(rp,x,y);
	    Text(rp, (char *)unit_str, 3);
	    x+=48;
	    Move(rp,x,y);
	    Text(rp, inq_res->vendor, 8);
	    x+=96;
	    Move(rp,x,y);
	    Text(rp, inq_res->product, 16);
	    x+=176;
	    Move(rp,x,y);
	    Text(rp, inq_res->revision, 4);
	    x+=48;
	    Move(rp,x,y);
	    const char *dtype=devtype_str(inq_res->device & SID_TYPE);
	    Text(rp,dtype,strlen(dtype));
	    printf(" %-*.*s %-*.*s %-*.*s %-7s\n",
               sizeof (inq_res->vendor),
               sizeof (inq_res->vendor),
               trim_spaces(inq_res->vendor, sizeof (inq_res->vendor)),
               sizeof (inq_res->product),
               sizeof (inq_res->product),
               trim_spaces(inq_res->product, sizeof (inq_res->product)),
               sizeof (inq_res->revision),
               sizeof (inq_res->revision),
               trim_spaces(inq_res->revision, sizeof (inq_res->revision)),
               devtype_str(inq_res->device & SID_TYPE));

	    FreeMem(inq_res, sizeof (*inq_res));
	}

        scsi_read_capacity_10_data_t *cap10;
        cap10 = do_scsi_read_capacity_10(&tio, unit);
        if (cap10 != NULL) {
            uint ssize = *(uint32_t *) &cap10->length;
            uint cap   = (*(uint32_t *) &cap10->addr + 1) / 1000;
            uint cap_c = 0;  // KMGTPEZY
            if (cap > 100000) {
                cap /= 1000;
                cap_c++;
            }
            cap *= ssize;
            while (cap > 9999) {
                cap /= 1000;
                cap_c++;
            }
            printf("%5u %5u %cB\n", ssize, cap, "KMGTPEZY"[cap_c]);

            x+=76;
            Move(rp,x,y);
            if (ssize<1000)
                Text(rp," ",1);
            itoa(ssize,_itoabuf,10);
            Text(rp,_itoabuf,strlen(_itoabuf));

            x+=48;
            Move(rp,x,y);
            if(cap<1000)
                Text(rp," ",1);
            if(cap<100)
                Text(rp," ",1);
            itoa(cap,_itoabuf,10);
            Text(rp,_itoabuf,strlen(_itoabuf));
            const char caps[]="KMGTPEZY";
            Text(rp,&caps[cap_c],1);
            Text(rp,"B",1);
            FreeMem(cap10, sizeof (*cap10));
        }
        cnt++;
        safe_close((struct IOStdReq *)&tio);
        memset(&tio, 0, sizeof(tio));
    }

    return 0;
}

static const struct Rectangle disk_table[] =
{
    { 40, 58,560,118},
    { 42, 59, 49, 12}, // Unit
    { 42, 72, 49,103},
    { 92, 59, 95, 12}, // Vendor
    { 92, 72, 95,103},
    {188, 59,175, 12}, // Device
    {188, 72,175,103},
    {364, 59, 49, 12}, // Revision
    {364, 72, 49,103},
    {414, 59, 71, 12}, // Type
    {414, 72, 71,103},
    {486, 59, 47, 12}, // ssize
    {486, 72, 47,103},
    {534, 59, 64, 12}, // Cap
    {534, 72, 64,103}
};

static void disks_page(void)
{
    struct NewGadget ng;
    int i;
    ULONG tag;
    page_header(&ng, "A4091 Diagnostics - Disks");

    SetRGB4(&screen->ViewPort,3,6,8,11);

    SetAPen(&screen->RastPort,2);
    Print("Unit  Vendor      Device                Rev.  Type     Blk   Size",52,68,FALSE);
    SetAPen(&screen->RastPort,1);

    ng.ng_LeftEdge   = 400;
    ng.ng_TopEdge    = 185;
    ng.ng_Width      = 120;
    ng.ng_GadgetText = "Back";
    ng.ng_GadgetID   = DISKS_BACK_ID;
    LastAdded = create_gadget(BUTTON_KIND);

    tag=GTBB_Recessed;
    for (i = 0; i < 15; i++)
    {
        DrawBevelBox(&screen->RastPort,disk_table[i].MinX,disk_table[i].MinY,
                                       disk_table[i].MaxX+1,
                                       disk_table[i].MaxY+1,
                                       GT_VisualInfo,  visualInfo,
                                       tag,            TRUE,
                                       TAG_DONE);
        tag = TAG_IGNORE;
    }

    page_footer();

    scan_disks();
}

static void debug_page(void)
{
    struct NewGadget ng;
    page_header(&ng, "A4091 Diagnostics - Debug");

    SetRGB4(&screen->ViewPort,3,6,8,11);
    SetAPen(&screen->RastPort,3);
    Print("DEBUG - TBD",0,120,TRUE);

    ng.ng_LeftEdge   = 400;
    ng.ng_TopEdge    = 145;
    ng.ng_Width      = 120;
    ng.ng_GadgetText = "Back";
    ng.ng_GadgetID   = DEBUG_BACK_ID;
    LastAdded = create_gadget(BUTTON_KIND);

    page_footer();
}

struct drawing {
    BYTE type, pen; // 1: rectangle filled 2: rectangle empty -1: end
    USHORT x, y, w, h;
};

static const struct drawing card[] = {
    { 1, 3,   0,   0, 430, 145 }, // card
    { 1, 3,  57, 145, 163,  10 }, // zslot
    { 1, 2, 431,  16,   1, 155 }, // bracket
    { 1, 2, 431,  16,  12,   2 },
    { 2, 2, 223,  14, 187, 128 }, // silkscreen
    { 1, 0, 270,  64,  90,  30 }, // cutout
    { 2, 1, 414,  41,  16,  56 }, // connectors back
    { 2, 1, 415, 108,  15,  33 },
    { 1, 1,  12,  12,  18,  18 }, // U304 (GALs)
    { 1, 1,  12,  38,  18,  18 }, // U203
    { 1, 1,  12,  64,  18,  18 }, // U305
    { 1, 1,  38,  12,  18,  18 }, // U207
    { 1, 1,  38,  38,  18,  18 }, // U306
    { 1, 1,  64,  12,  18,  18 }, // U205
    { 1, 1,  64,  38,  18,  18 }, // U202
    { 1, 1,  64,  64,  18,  18 }, // U303
    { 1, 1,  88,   6,   2,  10 }, // J100
    { 1, 1,  97,  13,  18,  44 }, // U206 (ROM)
    { 1, 1, 126,   4,  80,   8 }, // CN309
    { 1, 1, 132,  30,  42,  44 }, // U300 (LSI)
    { 1, 2,  38,  80,  18,  26 }, // U301 (OSC)
    { 1, 1,  50,  66,   6,  10 }, // U302
    { 1, 1,  23, 116,  11,  16 }, // U109
    { 1, 1,  62,  90,  10,  16 }, // U200
    { 1, 1,  70, 116,  10,  16 }, // U201
    { 1, 1,  97,  88,  10,  18 }, // U105
    { 1, 1,  96, 116,  10,  18 }, // U100
    { 1, 1, 122,  88,  10,  18 }, // U108
    { 1, 1, 122, 116,  10,  18 }, // U101
    { 1, 1, 140,  88,  10,  18 }, // U106
    { 1, 1, 142, 116,  10,  18 }, // U102
    { 1, 1, 160,  90,  10,  16 }, // U204
    { 1, 1, 160, 116,  10,  18 }, // U107
    { 1, 1, 184,  90,  10,  16 }, // U213
    { 1, 1, 182, 118,  10,  16 }, // U103
    { 1, 1, 208,  90,   6,  10 }, // U307
    { 1, 1, 190,  28,   8,  13 }, // U311
    { 1, 1, 188,  47,   8,  13 }, // U310
    { 1, 2, 198, 114,   2,  24 }, // power connector
    { 1, 2, 201, 116,   0,  20 },
    { 3, 2,  57, 145, 163,  10 }  // the end
};

static void draw_card(const struct drawing c[], int length)
{
    struct RastPort *rp = &screen->RastPort;
    int x=106, y=50, i,j;

    for (i=0; i<length; i++) {
        struct drawing d = c[i];
        switch (d.type) {
        case 1:
            SetAPen(rp, d.pen);
            RectFill(rp, x + d.x,
                     y + d.y/2,
                     x + d.x + d.w,
                     y + d.y/2 + d.h/2
            );
            break;
        case 2:
            SetAPen(rp, d.pen);
            Move(rp, x + d.x, y + d.y/2);
            Draw(rp, x + d.x + d.w, y + d.y/2);
            Draw(rp, x + d.x + d.w, y + d.y/2 + d.h/2);
            Draw(rp, x + d.x, y + d.y/2 + d.h/2);
            Draw(rp, x + d.x, y + d.y/2);
            break;
        case 3:
            SetAPen(rp, 2);
            for(j=d.x; j<(d.x + d.w); j+=4) {
                RectFill(rp, x+j, y + d.y/2, x+j+1, y + d.y/2 + d.h/2);
            }
        }
    }
}

static void main_page(void)
{
    struct NewGadget ng;

    SetRGB4(&screen->ViewPort,3,6,8,11);
    page_header(&ng, "A4091 Early Startup Menu");

    draw_card(card, ARRAY_LENGTH(card));

    ng.ng_LeftEdge   = 140;
    ng.ng_TopEdge    = 150;
    ng.ng_Width      = 175;
    ng.ng_GadgetText = "Disks";
    ng.ng_GadgetID   = MAIN_DISKS_ID;
    LastAdded = create_gadget(BUTTON_KIND);

    ng.ng_TopEdge= 170;
    ng.ng_GadgetText = "DIP Switches";
    ng.ng_GadgetID   = MAIN_DIPSWITCH_ID;
    LastAdded = create_gadget(BUTTON_KIND);

    ng.ng_TopEdge= 150;
    ng.ng_LeftEdge   = 325;
    ng.ng_GadgetText = "About";
    ng.ng_GadgetID   = MAIN_ABOUT_ID;
    LastAdded = create_gadget(BUTTON_KIND);

    ng.ng_TopEdge= 170;
    ng.ng_GadgetText = "Debug";
    ng.ng_GadgetID   = MAIN_DEBUG_ID;
    LastAdded = create_gadget_custom(BUTTON_KIND,
                                     GA_Disabled, TRUE,
                                     TAG_DONE);

    ng.ng_TopEdge= 200;
    ng.ng_GadgetText = "Boot";
    ng.ng_GadgetID   = MAIN_BOOT_ID;
    LastAdded = create_gadget(BUTTON_KIND);

    DrawBevelBox(&screen->RastPort,125,140,390,55,
                                    GT_VisualInfo,  visualInfo,
                                    GTBB_Recessed,  TRUE,
                                    GTBB_FrameType, BBFT_RIDGE,
                                    TAG_DONE);
    page_footer();
}

static void event_loop(void)
{
    ULONG class;
    UWORD icode;
    struct IntuiMessage *msg;
    struct Gadget *gad;
    int running = TRUE;

    while (running) {
        WaitPort(window->UserPort);
        if ((msg = GT_GetIMsg(window->UserPort))) {
            class = msg->Class;
            icode = msg->Code;
            gad = msg->IAddress;
            GT_ReplyIMsg(msg);
            switch (class) {
            case IDCMP_RAWKEY:
                if (icode == 0x45) // key down ESC
                    running = FALSE;
                break;
            case IDCMP_GADGETUP:
                switch (gad->GadgetID)
                {
                case MAIN_DISKS_ID:
                    disks_page();
                    break;
                case MAIN_DIPSWITCH_ID:
                    dipswitch_page();
                    break;
                case MAIN_ABOUT_ID:
                    about_page();
                    break;
                case MAIN_DEBUG_ID:
                    debug_page();
                    break;
                case DISKS_BACK_ID:
                case DIPSWITCH_BACK_ID:
                case ABOUT_BACK_ID:
                case DEBUG_BACK_ID:
                    main_page();
                    break;
                case MAIN_BOOT_ID:
                    running=FALSE;
                    break;
                }
            }
        }
    }
}

void boot_menu(void)
{
    SysBase = *(struct ExecBase **)4UL;
    printf("Bootmenu:\n");
    // Hack!?
    InitResident(FindResident("gadtools.library"), 0);

    IntuitionBase = OpenLibrary("intuition.library", 0);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library",0);
    GadToolsBase  = OpenLibrary("gadtools.library",36);

    if (!((1<<6) & *((volatile char *)0xBFE001))) {
	    printf("LMB pressed.\n");
        return;
    }
    if (((1<<10) & *((volatile UWORD *)0xDFF016))) {
	    printf("RMB mouse not pressed.\n");
	return;
    }

    printf("Bootmenu: enter\n");
    init_bootmenu();
    main_page();
    event_loop();
    cleanup_bootmenu();
    printf("Bootmenu: exit\n");
    return;
}

