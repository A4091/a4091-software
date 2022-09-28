#ifdef DEBUG_RDB
#define USE_SERIAL_OUTPUT
#endif

#include "port.h"
#include "printf.h"
#include <exec/types.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <devices/hardblocks.h>
#include <libraries/expansion.h>
#include <proto/expansion.h>
#include <resources/filesysres.h>
#include <dos/filehandler.h>

#include <stdint.h>
#include <string.h>
#include "cmdhandler.h"
#include "scsipiconf.h"
#include "scsimsg.h"
#include "reloc.h"
#include "rdb_partitions.h"
#include "ndkcompat.h"

#include "device.h"
#include "attach.h"

#define MAX_BLOCK_SIZE 2048

extern a4091_save_t *asave;
extern char real_device_name[];
extern int blksize;

struct FileSysResource *FileSysResBase = NULL;

struct MountData {
    struct IOStdReq     *ioreq;
    struct Library      *device;
    struct ConfigDev    *configDev;
    uint8_t             *block;
    unsigned int        unit;
    ULONG               PartitionList;
    ULONG               FileSysHeaderList;
};

void
find_partitions(struct MountData *md)
{
    int cur_partition = 0;
    uint8_t tmp;

    if (md->PartitionList == 0 || md->PartitionList == 0xffffffff) {
        printf("No partitions on disk.\n");
        return;
    }

    uint32_t cur_block = md->PartitionList;

next_partition:
#ifdef DEBUG
    printf("\nBlock: %d\n", cur_block);
#endif
    sdcmd_read_blocks(md->ioreq, md->block, cur_block, 1);

    struct PartitionBlock *pb = (struct PartitionBlock *)md->block;
    if (pb->pb_ID != IDNAME_PARTITION) {
        printf("Not a valid partition: %d\n", pb->pb_ID);
        return;
    }

    tmp = pb->pb_DriveName[0];
    pb->pb_DriveName[tmp + 1] = 0x00;

#ifdef DEBUG
    printf(" %8d\t%s\n", cur_partition, (char*)pb->pb_DriveName + 1);
    printf(" %08x\t%08x\n", pb->pb_ChkSum, pb->pb_HostID);
    printf(" %08x\t%08x\n", pb->pb_Flags, pb->pb_DevFlags);
#else
    printf(" Part %2d: %s ", cur_partition, (char*)pb->pb_DriveName + 1);
#endif

    char *execName = real_device_name;
    char* dosName = pb->pb_DriveName + 1;

    ULONG parmPkt[] = {
        (ULONG) dosName,
        (ULONG) execName,
        md->unit,           /* unit number */
        0,                  /* OpenDevice flags */
        0,0,0,0,0,
        0,0,0,0,0,
        0,0,0,0,0,
        0,0,
    };

    for (int i=0; i<17; i++) {
      parmPkt[4+i] = pb->pb_Environment[i];
    }

#ifdef DEBUG
    printf("TblSize: %d ", (uint32_t)parmPkt[4]);
#endif
    printf("LoCyl: %4d HiCyl: %4d DosType: %08x\n",
            (uint32_t)parmPkt[4+9], (uint32_t)parmPkt[4+10], (uint32_t)parmPkt[4+16]);

    struct DeviceNode *node = MakeDosNode(parmPkt);

    if (node) {
#ifdef DEBUG
        printf(" GlobalVec: %08x\n", (uint32_t)node->dn_GlobalVec);
#endif

      node->dn_GlobalVec = -1; // yet unclear if needed

      AddBootNode(0, 0, node, md->configDev);
#ifdef DEBUG
      printf("AddBootNode done.\n");
#endif
    }

    if (pb->pb_Next != 0xFFFFFFFF) {
        uint64_t next = pb->pb_Next;
        cur_block = next;

        cur_partition++;
        goto next_partition;
    }
    printf("No more partitions.\n");

    return;
}



static struct FileSysEntry *scan_filesystems(void)
{
    struct FileSysEntry *fse, *cdfs=NULL;
#ifdef DEBUG_RDB
    int x;
#endif

    /* NOTE - you should actually be in a Forbid while accessing any
     * system list for which no other method of arbitration is available.
     * However, for this example we will be printing the information
     * (which would break a Forbid anyway) so we won't Forbid.
     * In real life, you should Forbid, copy the information you need,
     * Permit, then print the info.
     */
    if (!(FileSysResBase = (struct FileSysResource *)OpenResource(FSRNAME))) {
        printf("Cannot open %s\n",FSRNAME);
    } else {
        printf("DosType   Version   Creator\n");
        printf("------------------------------------------------\n");
        for ( fse = (struct FileSysEntry *)FileSysResBase->fsr_FileSysEntries.lh_Head;
              fse->fse_Node.ln_Succ;
              fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ) {
#ifdef DEBUG_RDB
            for (x=24; x>=8; x-=8)
                putchar((fse->fse_DosType >> x) & 0xFF);

            putchar((fse->fse_DosType & 0xFF) < 0x30
                            ? (fse->fse_DosType & 0xFF) + 0x30
                            : (fse->fse_DosType & 0xFF));
#endif
            printf("      %s%d",(fse->fse_Version >> 16)<10 ? " " : "", (fse->fse_Version >> 16));
            printf(".%d%s",(fse->fse_Version & 0xFFFF), (fse->fse_Version & 0xFFFF)<10 ? " " : "");
            printf("     %s",fse->fse_Node.ln_Name);

            if (fse->fse_DosType==0x43443031) {
                cdfs=fse;
#ifndef ALL_FILESYSTEMS
                break;
#endif
            }


        }
    }
    return cdfs;
}

int add_cdromfilesystem(void)
{
    uint32_t cdfs_seglist=0;
    struct Resident *r;

    r=FindResident("cdfs");
    if (r == NULL) {
        int i;
        printf("No CDFileSystem in Kickstart ROM, search A4091 ROM\n");
        cdfs_seglist = relocate(asave->romfile[1], (uint32_t)asave->as_addr);
        if (cdfs_seglist == 0) {
                printf("Not found. Bailing out. (rErrno=%x)\n", rErrno);
                return 0;
        }
        printf("Scanning Filesystem...\n");
        for (i=cdfs_seglist; i<cdfs_seglist+0x400; i+=2)
            if(*(uint16_t *)i == 0x4afc)
                       break;

        if (*(uint16_t *)i == 0x4afc)
            r = (struct Resident *)i;
    }
    if (r) {
        printf("Initializing resident filesystem @ %p\n", r);
        InitResident(r, cdfs_seglist);
    } else {
	printf("No resident filesystem.\n");
    }
    return (r!=NULL);
}

void add_cdrom(struct MountData *md)
{
    struct FileSysEntry *fse=NULL;
    char *execName = real_device_name;
    char dosName[] = "CD0";

    ULONG parmPkt[] = {
        (ULONG) dosName,
        (ULONG) execName,
        md->unit,      /* unit number */
        0,             /* OpenDevice flags */
        17,            // de_TableSize
        2048>>2,       // de_SizeBlock
        0,             // de_SecOrg
        1,             // de_Surfaces
        1,             // de_SectorPerBlock
        1,             // de_BlocksPerTrack
        0,             // de_Reserved
        0,             // de_PreAlloc
        0,             // de_Interleave
        0,             // de_LowCyl
        0,             // de_HighCyl
        5,             // de_NumBuffers
        1,             // de_BufMemType
        0x100000,      // de_MaxTransfer
        0x7FFFFFFE,    // de_Mask
        2,             // de_BootPri
        0x43443031,    // de_DosType = "CD01"
    };

    if (add_cdromfilesystem())
        fse=scan_filesystems();

    struct DeviceNode *node = MakeDosNode(parmPkt);

    if (!node) {
        printf("Could not create DosNode\n");
        return;
    }
    if (!fse) {
        printf("Could not load filesystem\n");
        return;
    }

    // Process PatchFlags. Thank you, Toni.
    ULONG *dstPatch = &node->dn_Type;
    ULONG *srcPatch = &fse->fse_Type;
    ULONG patchFlags = fse->fse_PatchFlags;
    while (patchFlags) {
        if (patchFlags & 1) {
            *dstPatch = *srcPatch;
        }
        patchFlags >>= 1;
        srcPatch++;
        dstPatch++;
    }

    AddBootNode(2, ADNF_STARTPROC, node, md->configDev);
}

int
parse_rdb(struct MountData *md)
{
    int i, j;
    printf("Looking for RDB!\n");

    for (i=0; i<7; i++) { // FIXME LUNs?
        int found = 0;
        struct RigidDiskBlock *rdb = (struct RigidDiskBlock *)md->block;

        md->unit = i;

        if (safe_open(md->ioreq, i))
            continue;

        for (j = 0; j < RDB_LOCATION_LIMIT; j++) {
            sdcmd_read_blocks(md->ioreq, md->block, j, 1);
            if(rdb->rdb_ID == IDNAME_RIGIDDISK) {
                found=1;
                break;
            }
        }

        if (found) {
            printf("Unit %d: ", i);
            printf("RDB found at block %d\n", j);
#ifdef DEBUG
            printf("\n Cylinders:\t%d", rdb->rdb_Cylinders);
            printf("\n Heads:\t\t%d", rdb->rdb_Heads);
            printf("\n Sectors:\t%d", rdb->rdb_Sectors);
            printf("\n BlockBytes:\t%d", rdb->rdb_BlockBytes);
            printf("\n BadBlockList:\t%x", (uint32_t)rdb->rdb_BadBlockList);
            printf("\n PartitionList:\t%x", (uint32_t)rdb->rdb_PartitionList);
            printf("\n FileSysHdrLst:\t%x", (uint32_t)rdb->rdb_FileSysHeaderList);
            printf("\n DriveInit:\t%x\n", (uint32_t)rdb->rdb_DriveInit);
#endif
            md->PartitionList = rdb->rdb_PartitionList;
            md->FileSysHeaderList = rdb->rdb_FileSysHeaderList;

            find_partitions(md);
        } else {
            // Not a sufficient test but good
            // enough for a proof of concept?
            if (asave->cdrom_boot && blksize == 2048)
                add_cdrom(md);
            else
                printf("RDB not found!\n");
        }

        safe_close(md->ioreq);
        memset(md->ioreq, 0, sizeof(*(md->ioreq)));
    }
    return 0;
}

int mount_drives(struct ConfigDev *cd, struct Library *dev)
{
    int ret = 1;
    struct MountData *md = AllocMem(sizeof(struct MountData), MEMF_CLEAR | MEMF_PUBLIC);
    if (!md)
        goto goodbye;

    struct IOStdReq ior;

    md->block = AllocMem(MAX_BLOCK_SIZE, MEMF_PUBLIC);
    if (!md->block) {
        printf("RDB: Out of memory\n");
        goto goodbye;
    }

    md->ioreq = &ior;
    md->configDev = cd;
    md->device = dev;

    ret = parse_rdb(md);

goodbye:
    if(md && md->block)
        FreeMem(md->block, MAX_BLOCK_SIZE);
    if (md)
        FreeMem(md, sizeof(struct MountData));
    return ret;
}
