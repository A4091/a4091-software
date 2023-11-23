#ifdef DEBUG_DEVICE
#define USE_SERIAL_OUTPUT
#endif
#include "port.h"
#include "printf.h"
#include <stdlib.h>
#include <stdio.h>
#include "device.h"
#include "attach.h"
#include "reloc.h"
#include <resources/filesysres.h>
#include "version.h"
#include "romfile.h"

extern const char cdfs_id_string[];

typedef struct {
	uint32_t romfile[3], romfile_len[3], romfile_dostype[3];
} romfiles_t;

static uint32_t RomFetch32(uint32_t offset)
{
    uint8_t *rombase = (uint8_t *)asave->as_addr;
    uint32_t ret = 0;
    int i;
    for (i=0; i<16; i+=2) {
        ret <<=4;
        ret |= rombase[offset*4 +i] >>4;
    }
    return ret;
}

static void parse_romfiles(romfiles_t *rom)
{
    int i;

    rom->romfile_len[0] = 0;

    for (i=1; i<=2; i++) {
        /* Look for end-of-rom signature */
        if (RomFetch32((i*32*1024)-8) == 0xffff5352 &&
                RomFetch32((i*32*1024)-4) == 0x2f434448) {

            rom->romfile_len[0]=RomFetch32((i*32*1024) - 12);
            if (rom->romfile_len[0])
                rom->romfile[0]=RomFetch32((i*32*1024) - 16);

            rom->romfile_len[1]=RomFetch32((i*32*1024) - 20);
            if (rom->romfile_len[1]) {
                rom->romfile[1]=RomFetch32((i*32*1024) - 24);
                rom->romfile_dostype[1]=RomFetch32((i*32*1024) - 28);
            }

            rom->romfile_len[2]=RomFetch32((i*32*1024) - 32);
            if (rom->romfile_len[2]) {
                rom->romfile[2]=RomFetch32((i*32*1024) - 36);
                rom->romfile_dostype[2]=RomFetch32((i*32*1024) - 40);
            }

            break;
        }
    }

    printf("Detected %dkB ROM.\n", i*32);
    if(rom->romfile_len[0]) {
        printf("  Driver @ 0x%05x (%d bytes)\n", rom->romfile[0], rom->romfile_len[0]);

        for (i=1; i<3; i++) {
            if(rom->romfile_len[i]) {
                printf("  FS %d   @ 0x%05x (%d bytes): %08x\n", i, rom->romfile[i],
                            rom->romfile_len[i], rom->romfile_dostype[i]);
                if (RomFetch32(rom->romfile[i]) == 0x524e4301) {
                    rom->romfile_len[i] = RomFetch32(rom->romfile[i] + 4);
                    printf("            compressed (%d bytes)\n",
                                    rom->romfile_len[i]);
                }
            } else
                printf("  FS %d not found.\n", i);
        }
    } else
        printf("  Driver not found. Huh?\n");
}

static int add_cdromfilesystem(romfiles_t *rom)
{
    uint32_t cdfs_seglist = 0;
    struct Resident *r = NULL;
    struct FileSysResource *FileSysResBase;
    struct FileSysEntry *fse;

    printf("CDFS in Kickstart... ");
    r=FindResident("cdfs");
    if (r == NULL) {
        int i;
        printf("not found.\nCDFS in A4091 ROM... ");

        if (rom->romfile_len[1])
            cdfs_seglist = relocate(rom->romfile[1], (uint32_t)asave->as_addr);

        printf("%sfound.\n", cdfs_seglist?"":"not ");
        // baserel does not like rErrno
        if (cdfs_seglist == 0)
		return 0;

        printf("Resident struct... ");
        for (i=cdfs_seglist; i<cdfs_seglist + rom->romfile_len[1]; i+=2) {
            if(*(uint16_t *)i == 0x4afc) {
                r = (struct Resident *)i;
                break;
            }
        }
    }

    printf("%sfound.\n", r?"":"not ");

    if (r != NULL) {
        if (r && r->rt_Init) {
            printf("Initializing CDFS @%p... ", r);
            InitResident(r, cdfs_seglist);
            printf("done.\n");
	    return 1;
        } else
            printf("No rt_Init.\n");
    }

    FileSysResBase = (struct FileSysResource *)OpenResource(FSRNAME);
    if (!FileSysResBase)
	return 0;

    fse = AllocMem(sizeof(struct FileSysEntry), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fse)
	return 0;

    fse->fse_Node.ln_Name = (UBYTE*)cdfs_id_string;
    fse->fse_DosType = rom->romfile_dostype[1];
    fse->fse_Version = ((LONG)DEVICE_VERSION) << 16 | DEVICE_REVISION;
    fse->fse_PatchFlags = 0x190; // SegList and GlobalVec
    fse->fse_SegList = cdfs_seglist >> 2;
    fse->fse_GlobalVec = -1;
    //fse->fse_StackSize = 5120;
    fse->fse_StackSize = 16384; // Is there a right answer here?
    fse->fse_Priority = 10;

    Forbid();
    AddHead(&FileSysResBase->fsr_FileSysEntries,&fse->fse_Node);
    Permit();

    return (1);
}

void init_romfiles(void)
{
	romfiles_t rom;

	parse_romfiles(&rom);
	add_cdromfilesystem(&rom);
}
