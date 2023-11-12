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

void parse_romfiles(void)
{
    int i;

    for (i=1; i<=2; i++) {
	/* Look for end-of-rom signature */
        if (RomFetch32((i*32*1024)-8) == 0xffff5352 &&
                RomFetch32((i*32*1024)-4) == 0x2f434448) {

            asave->romfile_len[0]=RomFetch32((i*32*1024) - 12);
	    if (asave->romfile_len[0])
                asave->romfile[0]=RomFetch32((i*32*1024) - 16);

            asave->romfile_len[1]=RomFetch32((i*32*1024) - 20);
	    if (asave->romfile_len[1])
                asave->romfile[1]=RomFetch32((i*32*1024) - 24);

            printf("Detected %dkB ROM.\n", i*32);
	    if(asave->romfile_len[0]) {
                printf("  Driver @ 0x%05x (%d bytes)\n", asave->romfile[0],
				asave->romfile_len[0]);
	    } else
                printf("  Driver not found. Huh?\n");

	    if(asave->romfile_len[1]) {
                printf("  CDFS   @ 0x%05x (%d bytes)\n", asave->romfile[1],
				asave->romfile_len[1]);
		if (RomFetch32(asave->romfile[1]) == 0x524e4301) {
		    asave->romfile_len[1] = RomFetch32(asave->romfile[1] + 4);
		    printf("            compressed (%d bytes)\n",
				    asave->romfile_len[1]);
		}
	    } else
                printf("  CDFS not found.\n");

            break;
        }
    }
}

extern const char cdfs_id_string[];

int add_cdromfilesystem(void)
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

        if (asave->romfile_len[1])
            cdfs_seglist = relocate(asave->romfile[1], (uint32_t)asave->as_addr);

        printf("%sfound.\n", cdfs_seglist?"":"not ");
        // baserel does not like rErrno
        if (cdfs_seglist == 0)
		return 0;

        printf("Resident struct... ");
        for (i=cdfs_seglist; i<cdfs_seglist + asave->romfile_len[1]; i+=2) {
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
    fse->fse_DosType = 0x43443031;
    fse->fse_Version = ((LONG)DEVICE_VERSION) << 16 | DEVICE_REVISION;
    fse->fse_PatchFlags = 0x190; // SegList and GlobalVec
    fse->fse_SegList = cdfs_seglist >> 2;
    fse->fse_GlobalVec = -1;
    fse->fse_StackSize = 5120;
    fse->fse_Priority = 10;

    Forbid();
    AddHead(&FileSysResBase->fsr_FileSysEntries,&fse->fse_Node);
    Permit();

    return (1);
}
