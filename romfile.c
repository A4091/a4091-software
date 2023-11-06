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
		if (RomFetch32(asave->romfile[1]) == 0x524e4301)
		    printf("            compressed (%d bytes)\n",
				    RomFetch32(asave->romfile[1] + 4));
	    } else
                printf("  CDFS not found.\n");

            break;
        }
    }
}

int add_cdromfilesystem(void)
{
    uint32_t cdfs_seglist = 0;
    struct Resident *r;

    printf("CDFS in Kickstart... ");
    r=FindResident("cdfs");
    if (r == NULL) {
        int i;
        printf("Not found\nCDFS in A4091 ROM... ");

	if (asave->romfile_len[1])
            cdfs_seglist = relocate(asave->romfile[1], (uint32_t)asave->as_addr);

        if (cdfs_seglist == 0) {
            // baserel does not like rErrno
            printf("Not found\nToo bad.\n");
            return 0;
        }
        printf("Found\nResident struct... ");
        for (i=cdfs_seglist; i<cdfs_seglist + asave->romfile_len[1]; i+=2)
            if(*(uint16_t *)i == 0x4afc)
                       break;

        if (*(uint16_t *)i == 0x4afc)
            r = (struct Resident *)i;
    }

    if (r) {
        printf("Found\nInitializing CDFS @%p... ", r);
        InitResident(r, cdfs_seglist);
        printf("Done\n");
    } else {
        printf("Not found\nToo bad.\n");
    }
    return (r!=NULL);
}
