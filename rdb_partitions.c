#ifdef DEBUG_RDB
#define USE_SERIAL_OUTPUT
#endif

#include "port.h"
#include "printf.h"
#include <exec/types.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <libraries/expansion.h>
#include <proto/expansion.h>
#include <stdint.h>
#include <string.h>
#include "cmdhandler.h"
#include "scsipiconf.h"
#include "scsimsg.h"
#include "rdb_partitions.h"

// Based on https://github.com/captain-amygdala/pistorm/blob/main/platforms/amiga/piscsi/piscsi.h

#define RDB_BLOCK_LIMIT 16
#define MAX_BLOCK_SIZE 2048
// RDSK
#define RDB_IDENTIFIER 0x5244534B
// PART
#define PART_IDENTIFIER 0x50415254
// FSHD
#define	FS_IDENTIFIER 0x46534844

struct RigidDiskBlock {
    uint32_t   rdb_ID;
    uint32_t   rdb_SummedLongs;
    int32_t    rdb_ChkSum;
    uint32_t   rdb_HostID;
    uint32_t   rdb_BlockBytes;
    uint32_t   rdb_Flags;
    /* block list heads */
    uint32_t   rdb_BadBlockList;
    uint32_t   rdb_PartitionList;
    uint32_t   rdb_FileSysHeaderList;
    uint32_t   rdb_DriveInit;
    uint32_t   rdb_Reserved1[6];
    /* physical drive characteristics */
    uint32_t   rdb_Cylinders;
    uint32_t   rdb_Sectors;
    uint32_t   rdb_Heads;
    uint32_t   rdb_Interleave;
    uint32_t   rdb_Park;
    uint32_t   rdb_Reserved2[3];
    uint32_t   rdb_WritePreComp;
    uint32_t   rdb_ReducedWrite;
    uint32_t   rdb_StepRate;
    uint32_t   rdb_Reserved3[5];
    /* logical drive characteristics */
    uint32_t   rdb_RDBBlocksLo;
    uint32_t   rdb_RDBBlocksHi;
    uint32_t   rdb_LoCylinder;
    uint32_t   rdb_HiCylinder;
    uint32_t   rdb_CylBlocks;
    uint32_t   rdb_AutoParkSeconds;
    uint32_t   rdb_HighRDSKBlock;
    uint32_t   rdb_Reserved4;
    /* drive identification */
    char    rdb_DiskVendor[8];
    char    rdb_DiskProduct[16];
    char    rdb_DiskRevision[4];
    char    rdb_ControllerVendor[8];
    char    rdb_ControllerProduct[16];
    char    rdb_ControllerRevision[4];
    char    rdb_DriveInitName[40];
};

struct PartitionBlock {
    uint32_t   pb_ID;
    uint32_t   pb_SummedLongs;
    int32_t    pb_ChkSum;
    uint32_t   pb_HostID;
    uint32_t   pb_Next;
    uint32_t   pb_Flags;
    uint32_t   pb_Reserved1[2];
    uint32_t   pb_DevFlags;
    uint8_t    pb_DriveName[32];
    uint32_t   pb_Reserved2[15];
    uint32_t   pb_Environment[20];
    uint32_t   pb_EReserved[12];
};

static uint32_t *_block; // shared storage for 1 block
extern char real_device_name[];

void
find_partitions(struct ConfigDev *cd, struct RigidDiskBlock *rdb, struct IOStdReq *ioreq, int unit)
{
    int cur_partition = 0;
    uint8_t tmp;
    uint32_t *block = _block;

    if (!rdb || rdb->rdb_PartitionList == 0) {
        printf("No partitions on disk.\n");
        return;
    }

    uint32_t cur_block = rdb->rdb_PartitionList;

next_partition:
#ifdef DEBUG
    printf("\nBlock: %d\n", cur_block);
#endif
    sdcmd_read_blocks(ioreq, (uint8_t*)block, cur_block, 1);

    uint32_t first = block[0];
    if (first != PART_IDENTIFIER) {
        printf("Not a valid partition: %d\n", first);
        return;
    }

    struct PartitionBlock *pb = (struct PartitionBlock *)block;
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
        unit,                  /* unit number */
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

      AddBootNode(0, 0, node, cd);
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

int
parse_rdb(struct ConfigDev *cd, struct Library *dev)
{
    int i, j;
    struct IOStdReq ior;

    _block = AllocMem(MAX_BLOCK_SIZE, MEMF_PUBLIC);
    if (!_block) {
        printf("RDB: Out of memory\n");
	return 1;
    }

    uint32_t *block=_block; // shared storage for 1 block

    printf("Looking for RDB!\n");

    for (i=0; i<7; i++) { // FIXME LUNs?
	int found = 0;

	if (safe_open(&ior, i))
	    continue;

        for (j = 0; j < RDB_BLOCK_LIMIT; j++) {
            sdcmd_read_blocks(&ior, (uint8_t*)block, j, 1);
            uint32_t first = block[0];
            if (first == RDB_IDENTIFIER) {
		found=1;
		break;
	    }
        }

	if (found) {
            struct RigidDiskBlock *rdb = (struct RigidDiskBlock *)block;
	    printf("Unit %d: ", i);
            printf("RDB found at block %d\n", j);
#ifdef DEBUG
            printf("\n Cylinders:\t%d", rdb->rdb_Cylinders);
            printf("\n Heads:\t\t%d", rdb->rdb_Heads);
            printf("\n Sectors:\t%d", rdb->rdb_Sectors);
            printf("\n BlockBytes:\t%d", rdb->rdb_BlockBytes);
            printf("\n PartitionList:\t%x\n", (uint32_t)rdb->rdb_PartitionList);
#endif

            find_partitions(cd, rdb, &ior, i);
	} else {
            printf("RDB not found!\n");
	}

        safe_close(&ior);
	memset(&ior, 0, sizeof(ior));
    }
    FreeMem(_block, MAX_BLOCK_SIZE);
    return 0;
}
