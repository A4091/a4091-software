#include <devices/trackdisk.h>
#include <dos/dostags.h>
#include <exec/errors.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/semaphores.h>
#include <exec/io.h>
#include <libraries/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <utility/tagitem.h>
#include <clib/alib_protos.h>
#include <devices/scsidisk.h>
#include <devices/hardblocks.h>
#include <dos/filehandler.h>
#include <resources/filesysres.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOUNTER_CMD_NOP     0
#define MOUNTER_CMD_EXIT    1
#define MOUNTER_CMD_SCAN    2 /* Scan and mount/unmount specified device */
                              /* one is whether to skip LUNs */

#define MOUNTER_STATUS_SUCCESS    0x00
#define MOUNTER_STATUS_UNHANDLED  0x01
#define MOUNTER_STATUS_FAILURE    0x02
#define MOUNTER_STATUS_UNKNOWN    0x03
#define MOUNTER_STATUS_TERMINATED 0x04

#define MOUNTER_SCAN_FLAG_MOUNT   0x01
#define MOUNTER_SCAN_FLAG_UNMOUNT 0x02
#define MOUNTER_SCAN_FLAG_NOLUNS  0x04

#define MOUNTER_PORT "mounter"

typedef struct {
    struct Message  header;
    uint8_t         cmd;
    uint8_t         unit_min;
    uint8_t         unit_max;
    uint8_t         lun_min;
    uint8_t         lun_max;
    uint8_t         status;
    uint8_t         scan_flags;
    uint32_t        open_flags;
    char           *devname;
} mounter_msg_t;

static void
usage(void)
{
    printf("Usage: -k kill daemon\n"
           "       -h show help\n"
           "       -m mount\n"
           "       -u unmount\n"
           "       -s start daemon\n"
           "       <device name> [<unit>]\n");
}

/* BCPL conversion functions */
#define BTOC(x) ((void *)(((ULONG)(x))<<2))
#define CTOB(x) (((ULONG)(x))>>2)

#if 0
static void
devlist(void)
{
    struct FileSysStartupMsg *startup;
    struct DosLibrary        *DosBase;
    struct RootNode          *rootnode;
    struct DosInfo           *dosinfo;
    struct DevInfo           *devinfo;
    struct DosEnvec          *envec;
    char                     *devname;
    char                     *pos;
    int                       notfound = 1;

    DosBase  = (struct DosLibrary *) OpenLibrary("dos.library", 0L);

    rootnode = DosBase->dl_Root;
    dosinfo  = (struct DosInfo *) BTOC(rootnode->rn_Info);
    devinfo  = (struct DevInfo *) BTOC(dosinfo->di_DevInfo);

printf("Devlist\n");
    while (devinfo != NULL) {
        if (devinfo->dvi_Type == DLT_DEVICE) {
            char *disk_device;
            uint disk_unit;
            uint disk_flags;
            devname = (char *) BTOC(devinfo->dvi_Name);
            printf("  %.*s\n", devname[0], devname + 1);
            startup = (struct FileSysStartupMsg *) BTOC(devinfo->dvi_Startup);
            disk_device = ((char *) BTOC(startup->fssm_Device)) + 1;
            disk_unit   = startup->fssm_Unit;
            disk_flags  = startup->fssm_Flags;
            envec       = (struct DosEnvec *) BTOC(startup->fssm_Environ);
            printf("   dev=%s unit=%u flags=%u startblk=%u maxblk=%u\n",
                   disk_device, disk_unit, disk_flags,
                   envec->de_LowCyl *
                   envec->de_Surfaces * envec->de_BlocksPerTrack,
                   (envec->de_HighCyl + 1) * envec->de_Surfaces *
                   envec->de_BlocksPerTrack);
        }

        devinfo = (struct DevInfo *) BTOC(devinfo->dvi_Next);
    }
printf("\n");


    CloseLibrary((struct Library *)DosBase);
}
#endif
static struct DevInfo *
already_mounted(const char *devname, uint unitno, uint lowcyl, uint highcyl)
{
    struct FileSysStartupMsg *startup;
    struct DosLibrary        *DosBase;
    struct RootNode          *rootnode;
    struct DosInfo           *dosinfo;
    struct DevInfo           *devinfo;
    struct DosEnvec          *env;
    char                     *name;

    DosBase  = (struct DosLibrary *) OpenLibrary("dos.library", 0L);

    rootnode = DosBase->dl_Root;
    dosinfo  = (struct DosInfo *) BTOC(rootnode->rn_Info);
    devinfo  = (struct DevInfo *) BTOC(dosinfo->di_DevInfo);

    for (; devinfo != NULL;
           devinfo = (struct DevInfo *) BTOC(devinfo->dvi_Next)) {
        if (devinfo->dvi_Type != DLT_DEVICE)
            continue;  // Not a device
        startup = (struct FileSysStartupMsg *) BTOC(devinfo->dvi_Startup);
        name = (char *) BTOC(startup->fssm_Device);
        if ((unitno != startup->fssm_Unit) ||
            (strcmp(name + 1, devname) != 0)) {
// printf("%s %d not match name/unit %s %d\n", name + 1, startup->fssm_Unit, devname, unitno);
            continue;  // Unit or device name doesn't match
        }
        env = (struct DosEnvec *) BTOC(startup->fssm_Environ);
        if ((env->de_HighCyl < lowcyl) || (env->de_LowCyl > highcyl))
{
// printf("%s %d outside cyl range %d %d %d %d\n", name + 1, startup->fssm_Unit, env->de_LowCyl, env->de_HighCyl, lowcyl, highcyl);
            continue;  // Not this partition
}
        break;
    }

    CloseLibrary((struct Library *)DosBase);

    return (devinfo);
}


static void
print_id(ULONG name)
{
    unsigned char ch;
    int index;

    for (index = 0; index < 4; index++) {
        ch = (name >> (8 * (3 - index))) & 255;
        if ((ch < 32) || (ch > 'Z'))
            printf("\\%d", ch);
        else
            printf("%c", ch);
    }
}

static LONG
rdb_chksum(void *ptr, uint32_t blksize)
{
    LONG size;
    LONG csum = 0;
    LONG *buf = (LONG *) ptr;

    size = buf[1];
    if (size > (blksize >> 2)) {
        printf("Invalid size of csum structure: %ld\n", size);
        return (1);
    }
    for (; size > 0; size--) {
        csum += *buf;
        buf++;
    }
    return (csum);
}

static ULONG
blk_read(struct IOExtTD *tio, uint32_t blk, uint32_t blksize, void *buf)
{
    tio->iotd_Req.io_Command = CMD_READ;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = blk * blksize;
    tio->iotd_Req.io_Length  = blksize;
    tio->iotd_Req.io_Data    = buf;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0;

//    printf("blkread %u\n", blk);
    return (DoIO((struct IORequest *) tio));
}

static void
mount_device(const char *name, const char *devname, uint8_t unitno,
             ULONG dostype)
{
    struct DosLibrary        *DosBase;
    struct FileSysEntry      *fse;
    struct FileSysResource   *fsr;
    struct FileSysStartupMsg *fsstart;
    struct DosList           *de;
    ULONG                    *envec;

    DosBase  = (struct DosLibrary *) OpenLibrary("dos.library", 0L);

    de = MakeDosEntry(name, DLT_DEVICE);
    if (de == NULL) {
        printf("Failed to alloc Dos Entry for %s\n", name);
        goto failed_dosentry;
    }
    de->dol_Type                           = DLT_DEVICE;
    de->dol_misc.dol_handler.dol_StackSize = 8192;
    de->dol_misc.dol_handler.dol_Priority  = 0;

#define ENVEC_SIZE 10
    fsstart = AllocVec(sizeof (*fsstart), MEMF_PUBLIC | MEMF_CLEAR);
    if (fsstart == NULL)
        goto failed_fsstart;
    envec   = AllocVec((ENVEC_SIZE + 1) * 4, MEMF_PUBLIC | MEMF_CLEAR);
    if (envec == NULL)
        goto failed_envec;

    fsstart->fssm_Unit    = unitno;
    fsstart->fssm_Device  = (ULONG) devname;
    fsstart->fssm_Environ = CTOB(envec);
    fsstart->fssm_Flags   = 0;

    de->dol_misc.dol_handler.dol_SegList =
        DOSBase->dl_Root->rn_FileHandlerSegment;

    fsr = OpenResource("FileSystem.resource");
    if (fsr == NULL)
        goto failed_openresource;
    Forbid();
    fse = (struct FileSysEntry *) fsr->fsr_FileSysEntries.lh_Head;
    while (fse->fse_Node.ln_Succ != NULL) {
        if (fse->fse_DosType == dostype) {
            if (fse->fse_PatchFlags & 0x0001)
                de->dol_Type = fse->fse_Type;
            if (fse->fse_PatchFlags & 0x0002)
                de->dol_Task = (struct MsgPort *)fse->fse_Task;
            if (fse->fse_PatchFlags & 0x0004)
                de->dol_Lock = fse->fse_Lock;
            if (fse->fse_PatchFlags & 0x0008)
                de->dol_misc.dol_handler.dol_Handler = fse->fse_Handler;
            if (fse->fse_PatchFlags & 0x0010)
                de->dol_misc.dol_handler.dol_StackSize = fse->fse_StackSize;
            if (fse->fse_PatchFlags & 0x0020)
                de->dol_misc.dol_handler.dol_Priority = fse->fse_Priority;
            if (fse->fse_PatchFlags & 0x0040)
                de->dol_misc.dol_handler.dol_Startup = fse->fse_Startup;
            if (fse->fse_PatchFlags & 0x0080)
                de->dol_misc.dol_handler.dol_SegList = fse->fse_SegList;
            if (fse->fse_PatchFlags & 0x0100)
                de->dol_misc.dol_handler.dol_GlobVec = fse->fse_GlobalVec;
            break;
        }
        fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ;
    }
    Permit();

    /* No need to CloseResource() */
failed_openresource:
    FreeVec(envec);
failed_envec:
    FreeVec(fsstart);
failed_fsstart:
    FreeDosEntry(de);
failed_dosentry:
    CloseLibrary((struct Library *)DosBase);
}


typedef struct {
    uint8_t  bootable;     // This partition is bootable
    uint8_t  s_head;       // Partition start: head
    uint8_t  s_seccyl[2];  // Partition start: encoded sector / cylinder

    uint8_t  type;         // Partition type
    uint8_t  e_head;       // Partition end: head
    uint8_t  e_seccyl[2];  // Partition end: encoded sector / cylinder

    uint32_t s_sector;     // Partition starting sector number of data
    uint32_t p_sectors;    // Partition size in sectors
} partition_ent_t;
#define PENT_OFFSET      0x1be  /* Offset into MBR of first partition entry */
typedef struct MBRBlock {
    uint8_t pad1[PENT_OFFSET];
    partition_ent_t part[4];
    uint8_t mbr_id[2];
} MBRBlock_t;
#define PART_TYPE_EXTENDED 0x05

static void
scan_partition_table_mbr(mounter_msg_t *msg, uint8_t unitno,
                         struct IOExtTD *tio,
                         struct DriveGeometry *dg, uint8_t *buf)
{
    MBRBlock_t *mbr = (MBRBlock_t *) buf;
    int primary;
    int part = 1;
    printf("MBR\n");
    for (primary = 0; primary < 4; primary++) {
        printf("P%d %c %3d %5d %5d",
               part, mbr->part[primary].bootable ? '*' : ' ',
               mbr->part[primary].type, mbr->part[primary].s_sector,
               mbr->part[primary].p_sectors);
        if (mbr->part[primary].type == PART_TYPE_EXTENDED)
            printf(" Ext");
        printf("\n");
        part++;
    }
}


#if 0
struct DevInfo {
    BPTR  dvi_Next;
    LONG  dvi_Type;
    APTR  dvi_Task;
    BPTR  dvi_Lock;
    BSTR  dvi_Handler;
    LONG  dvi_StackSize;
    LONG  dvi_Priority;
    LONG  dvi_Startup;
    BPTR  dvi_SegList;
    BPTR  dvi_GlobVec;
    BSTR  dvi_Name;
};
#endif

static int
Action_Die(struct MsgPort *msgport)
{
    int                    rc = 0;
    struct MsgPort        *replyport;
    struct StandardPacket *packet;

    replyport = (struct MsgPort *) CreatePort(NULL, 0);
    if (replyport == NULL) {
        printf("Failed to create reply port\n");
        return (1);
    }

    packet = (struct StandardPacket *)
         AllocMem(sizeof (struct StandardPacket), MEMF_CLEAR | MEMF_PUBLIC);

    if (packet == NULL) {
        printf("Failed to allocate memory\n");
        DeletePort(replyport);
        return (1);
    }

    packet->sp_Msg.mn_Node.ln_Name = (char *) &(packet->sp_Pkt);
    packet->sp_Pkt.dp_Link         = &(packet->sp_Msg);
    packet->sp_Pkt.dp_Port         = replyport;
    packet->sp_Pkt.dp_Type         = ACTION_DIE;
    packet->sp_Pkt.dp_Arg1         = 0;
    packet->sp_Pkt.dp_Arg2         = 0;
    packet->sp_Pkt.dp_Arg3         = 0;
    packet->sp_Pkt.dp_Arg4         = 0;

    PutMsg(msgport, (struct Message *) packet);

    WaitPort(replyport);
    GetMsg(replyport);

    if (packet->sp_Pkt.dp_Res1 == DOSFALSE)
        rc = packet->sp_Pkt.dp_Res2;
    else
        rc = 0;

    FreeMem(packet, sizeof (struct StandardPacket));
    DeletePort(replyport);

    return (rc);
}

static int
Action_Inhibit(struct MsgPort *msgport, int do_inhibit)
{
    int                    rc = 0;
    struct MsgPort        *replyport;
    struct StandardPacket *packet;

    replyport = (struct MsgPort *) CreatePort(NULL, 0);
    if (replyport == NULL) {
        printf("Failed to create reply port\n");
        return (1);
    }

    packet = (struct StandardPacket *)
         AllocMem(sizeof (struct StandardPacket), MEMF_CLEAR | MEMF_PUBLIC);

    if (packet == NULL) {
        printf("Failed to allocate memory\n");
        DeletePort(replyport);
        return (1);
    }

    packet->sp_Msg.mn_Node.ln_Name = (char *) &(packet->sp_Pkt);
    packet->sp_Pkt.dp_Link         = &(packet->sp_Msg);
    packet->sp_Pkt.dp_Port         = replyport;
    packet->sp_Pkt.dp_Type         = ACTION_INHIBIT;
    packet->sp_Pkt.dp_Arg1         = do_inhibit;
    packet->sp_Pkt.dp_Arg2         = 0;
    packet->sp_Pkt.dp_Arg3         = 0;
    packet->sp_Pkt.dp_Arg4         = 0;

    PutMsg(msgport, (struct Message *) packet);

    WaitPort(replyport);
    GetMsg(replyport);

    if (packet->sp_Pkt.dp_Res1 == DOSFALSE)
        rc = packet->sp_Pkt.dp_Res2;
    else
        rc = 0;

    FreeMem(packet, sizeof (struct StandardPacket));
    DeletePort(replyport);

    return (rc);
}

static void
unmount_device(struct DevInfo *di)
{
    int rc;
    /*
     * First remove the device from the DOS list, then send ACTION_DIE
     * to the filesystem handler. This should quiesce the filesystem so
     * that it's sufficiently "unmounted" and unavailable.
     */
    char *name = (char *) BTOC(di->dvi_Name) + 1;
    struct MsgPort *msgport = (struct MsgPort *) DeviceProc(name);
    if (msgport == NULL) {
        printf("Failed to find MessagePort of %s\n", name);
        return;
    }

    printf("Unmounting %s\n", name);

#if 1
    if ((rc = Action_Inhibit(msgport, DOSTRUE)) != 0)
        printf("Inhibit to %s failed: %d\n", name, rc);
    else
        printf("Inhibit sent to %s\n", name);
    system("dir sys:");
#endif
#if 1
    if ((rc = Action_Inhibit(msgport, DOSFALSE)) != 0)
        printf("Uninhibit to %s failed: %d\n", name, rc);
    else
        printf("Uninhibit sent to %s\n", name);
    system("dir sys:");
#endif

#if 0
    if ((rc = Action_Die(msgport)) != 0)
        printf("Die to %s failed: %d\n", name, rc);
    else
        printf("Die sent to %s\n", name);
#endif

// 202 = ERROR_OBJECT_IN_USE

#if 0
    /* XXX: Can I get message port directly from DevInfo? */

    /* Remove device from DOS list */
    struct DosList *DosList;
    DosList = LockDosList(LDF_DEVICES | LDF_WRITE);
    DosList = FindDosEntry(DosList, name, LDF_DEVICES);
    if (DosList != NULL) {
        RemDosEntry(DosList);
    }
    UnLockDosList(LDF_DEVICES | LDF_WRITE);
#if 0
    /*
     * WARN: Freeing the DOS node is probably not safe, as we don't
     *       know who else has a copy of it. This code will skip
     *       doing that.
     */
    // FreeVec(DosList);  // Freeing the node is probably unsafe
#endif

#endif
#if 0
    ACTION_DIE
#endif
}

static void
scan_partition_table_rdb(mounter_msg_t *msg, uint8_t unitno,
                         struct IOExtTD *tio, struct DriveGeometry *dg,
                         uint partblk, uint8_t *buf)
{
    uint32_t blksize = dg->dg_SectorSize;
    struct PartitionBlock *part = (struct PartitionBlock *) buf;
    struct DosEnvec       *env  = (struct DosEnvec *) part->pb_Environment;
    struct DevInfo        *di;
    uint8_t scan_flags = msg->scan_flags;

    while (partblk != -1) {
        if ((blk_read(tio, partblk, blksize, buf)) ||
            (part->pb_ID != IDNAME_PARTITION) ||
            (rdb_chksum(part, blksize))) {
            return;
        }

        printf("  ");
        print_id(part->pb_ID);
        printf("  Blk=%-3u DriveName=%-4.*s DevFlags=%-4lu HostID=%lu "
               "Next=%-3lu %s%s\n", partblk,
               (part->pb_DriveName[0] < 32) ? part->pb_DriveName[0] : 32,
               part->pb_DriveName + 1,
               part->pb_DevFlags, part->pb_HostID, part->pb_Next,
               (part->pb_Flags & PBFF_NOMOUNT) ? "      " : "Mount ",
               (part->pb_Flags & PBFF_BOOTABLE) ? "Boot" : "");
        printf("        DosType=%08lx ", env->de_DosType);
        print_id(env->de_DosType);
        di = already_mounted(msg->devname, unitno, env->de_LowCyl, env->de_HighCyl);
        if (di != NULL) {
            printf(" Mounted");
        }
        printf("\n");
        if ((scan_flags & MOUNTER_SCAN_FLAG_MOUNT) && (di == NULL)) {
            mount_device(part->pb_DriveName + 1, msg->devname, unitno,
                         env->de_DosType);
        } else if ((scan_flags & MOUNTER_SCAN_FLAG_UNMOUNT) && (di != NULL)) {
            unmount_device(di);
        }
        partblk = part->pb_Next;
    }
}

static void
scan_partition_table(mounter_msg_t *msg, uint8_t unitno, struct IOExtTD *tio,
                     struct DriveGeometry *dg)
{
    uint32_t blksize = dg->dg_SectorSize;
    uint32_t maxblk  = dg->dg_TotalSectors;
    struct RigidDiskBlock *rdb;
    MBRBlock_t            *mbr;
    uint rdbblk;
    uint8_t *buf = AllocMem(blksize, MEMF_PUBLIC);
    if (buf == NULL)
        return;

    rdb = (struct RigidDiskBlock *) buf;
    mbr = (MBRBlock_t *) buf;

    for (rdbblk = 0; rdbblk < RDB_LOCATION_LIMIT; rdbblk++) {
        if (blk_read(tio, rdbblk, blksize, buf))
            goto give_up;

        if ((rdb->rdb_ID == IDNAME_RIGIDDISK) &&
            (rdb_chksum(rdb, blksize) == 0)) {
            printf(" RDB=%u HostID=%lu BlockBytes=%lu Flags=0x%04lx\n",
                   rdbblk, rdb->rdb_HostID, rdb->rdb_BlockBytes,
                   rdb->rdb_Flags);
            scan_partition_table_rdb(msg, unitno, tio, dg,
                                     rdb->rdb_PartitionList, buf);
            goto give_up;
        }
        if ((mbr->mbr_id[0] == 0x55) && (mbr->mbr_id[1] == 0xaa) &&
            (mbr->part[0].s_sector < maxblk) &&
            (mbr->part[1].s_sector < maxblk) &&
            (mbr->part[2].s_sector < maxblk) &&
            (mbr->part[3].s_sector < maxblk) &&
            (mbr->part[0].p_sectors <= maxblk) &&
            (mbr->part[1].p_sectors <= maxblk) &&
            (mbr->part[2].p_sectors <= maxblk) &&
            (mbr->part[3].p_sectors <= maxblk)) {
            // XXX: Need to ignore unused partitions
            scan_partition_table_mbr(msg, unitno, tio, dg, buf);
            goto give_up;
        }
    }
    if (rdbblk >= RDB_LOCATION_LIMIT)
        goto give_up;

give_up:
    FreeMem(buf, blksize);
}

static uint8_t
mounter_scan_lun(mounter_msg_t *msg, uint8_t unitno, struct IOExtTD *tio)
{
    ulong failcode;
    uint8_t rc;
    struct DriveGeometry dg;
    char *devname = msg->devname;

    if (OpenDevice(devname, unitno, (struct IORequest *) tio, msg->open_flags))
        return (MOUNTER_STATUS_FAILURE);

    tio->iotd_Req.io_Command = TD_GETGEOMETRY;
    tio->iotd_Req.io_Actual  = 0;
    tio->iotd_Req.io_Offset  = 0;
    tio->iotd_Req.io_Length  = sizeof (dg);
    tio->iotd_Req.io_Data    = &dg;
    tio->iotd_Req.io_Flags   = 0;
    tio->iotd_Req.io_Error   = 0;
    failcode = DoIO((struct IORequest *) tio);
    if (failcode == 0) {
        printf("%s unit %-2d %lu sectors x %lu  C=%lu H=%lu S=%lu  Type=%u%s\n",
               devname, unitno,
               dg.dg_TotalSectors, dg.dg_SectorSize, dg.dg_Cylinders,
               dg.dg_Heads, dg.dg_TrackSectors, dg.dg_DeviceType,
               (dg.dg_Flags & DGF_REMOVABLE) ? " Removable" : "");
        rc = MOUNTER_STATUS_SUCCESS;
    } else {
        printf("TD_GETGEOMETRY fail %lu\n", failcode);
        rc = MOUNTER_STATUS_FAILURE;
    }
    if (((dg.dg_SectorSize & (dg.dg_SectorSize -1)) == 0) &&
        (dg.dg_SectorSize >= 256) && (dg.dg_SectorSize <= 8192)) {
        /* Is a power of two sector size between 256 and 8192 bytes */
        scan_partition_table(msg, unitno, tio, &dg);
    }

    CloseDevice((struct IORequest *) tio);
    return (rc);
}

static uint8_t
mounter_scan(mounter_msg_t *msg)
{
    struct IOExtTD *tio;
    struct MsgPort *mp;
    uint8_t rc = MOUNTER_STATUS_SUCCESS;
    uint unitbase;
    uint lun;

    mp = CreatePort(NULL, 0);
    if (mp == NULL) {
        printf("Failed to create message port\n");
        return (MOUNTER_STATUS_FAILURE);
    }

    tio = (struct IOExtTD *) CreateExtIO(mp, sizeof (struct IOExtTD));
    if (tio == NULL) {
        printf("Failed to create tio struct\n");
        rc = MOUNTER_STATUS_FAILURE;
        goto extio_fail;
    }

    for (unitbase = msg->unit_min; unitbase <= msg->unit_max; unitbase++) {
        for (lun = msg->lun_min; lun <= msg->lun_max; lun++) {
            if (mounter_scan_lun(msg, unitbase + 10 * lun, tio))
                break;  // Don't scan past lun with failure
        }
    }

    DeleteExtIO((struct IORequest *) tio);

extio_fail:
    DeletePort(mp);
    return (rc);
}

static void
close_mounter_port(void)
{
    mounter_msg_t  *msg;
    struct MsgPort *msgport;

    Forbid();
    while (msgport = FindPort(MOUNTER_PORT)) {
        while ((msg = (mounter_msg_t *) GetMsg(msgport)) != NULL) {
            msg->status = MOUNTER_STATUS_TERMINATED;
            ReplyMsg(&msg->header);
        }
        DeletePort(msgport);
    }
    Permit();
}

static void
start_mounter(void)
{
    ULONG           sigmask;
    ULONG           signals;
    struct MsgPort *msgport;
    mounter_msg_t  *msg;

    printf("Mounter daemon\n");
    Forbid();
    msgport = FindPort(MOUNTER_PORT);
    if (msgport != NULL) {
        Permit();
        printf("Mounter is already running\n");
        return;
    }
    msgport = CreatePort(MOUNTER_PORT, 0);
    Permit();

    sigmask = (1 << msgport->mp_SigBit) | SIGBREAKF_CTRL_C;
    while ((signals = Wait(sigmask)) & ~SIGBREAKF_CTRL_C) {
        while (msg = (mounter_msg_t *) GetMsg(msgport)) {
            switch (msg->cmd) {
                case MOUNTER_CMD_NOP:
                    printf("NOP\n");
                    msg->status = MOUNTER_STATUS_SUCCESS;
                    break;
                case MOUNTER_CMD_EXIT:
                    printf("Exit\n");
                    msg->status = MOUNTER_STATUS_SUCCESS;
                    ReplyMsg(&msg->header);
                    goto finish_exit;
                case MOUNTER_CMD_SCAN:
                    printf("Scan\n");
                    msg->status = mounter_scan(msg);
                    break;
                default:
                    printf("Unknown command %u\n", msg->cmd);
                    msg->status = MOUNTER_STATUS_UNKNOWN;
                    break;
            }
            ReplyMsg(&msg->header);
        }
    }
    printf("^C Abort\n");

finish_exit:
    close_mounter_port();
// Permit();  // XXX: Don't do the permit when exiting a created process
}

static uint8_t
notify_mounter(uint8_t cmd, uint8_t unit_min, uint8_t unit_max,
               uint8_t lun_min, uint8_t lun_max, uint8_t scan_flags,
               char *devname, uint32_t open_flags)
{
    struct MsgPort *msgport;
    mounter_msg_t   msg;
    ULONG           sigmask;
    ULONG           signals;

    msg.header.mn_Length       = sizeof (msg) - sizeof (struct Message);
    msg.header.mn_ReplyPort    = CreatePort(NULL, 0);
    msg.header.mn_Node.ln_Type = NT_MESSAGE;
    msg.cmd        = cmd;
    msg.unit_min   = unit_min;
    msg.unit_max   = unit_max;
    msg.lun_min    = lun_min;
    msg.lun_max    = lun_max;
    msg.status     = MOUNTER_STATUS_UNHANDLED;
    msg.scan_flags = scan_flags;
    msg.open_flags = open_flags;
    msg.devname    = devname;

    Forbid();
    msgport = (struct MsgPort *) FindPort(MOUNTER_PORT);
    if (msgport == NULL) {
        Permit();
        printf("Mounter daemon is not running\n");
        return (1);
    }
    PutMsg(msgport, &msg.header);
    Permit();

    sigmask = (1 << msg.header.mn_ReplyPort->mp_SigBit) | SIGBREAKF_CTRL_C;

    signals = Wait(sigmask) & SIGBREAKF_CTRL_C;
    if (signals == 0)
        WaitPort(msg.header.mn_ReplyPort);
    DeletePort(msg.header.mn_ReplyPort);
    return (msg.status);
}

static void
stop_mounter(void)
{
    uint8_t rc;
    rc = notify_mounter(MOUNTER_CMD_EXIT, 0, 0, 0, 0, 0, NULL, 0);
    if (rc != 0)
        printf("Failed to stop Mounter: %d\n", rc);
}

int
main(int argc, char *argv[])
{
    char *devname = NULL;
    char *unit_str = NULL;
    char *flags_str = NULL;
    char *lun_str = NULL;
    int arg;
    int flag_mount = 0;
    int flag_unmount = 0;
    int unit_min = -1;
    int unit_max = 7;
    int lun_min = -1;
    int lun_max = 7;
    uint32_t open_flags = 0;
    uint8_t scan_flags = 0;
    int pos;
    uint status;

    for (arg = 1; arg < argc; arg++) {
        char *ptr = argv[arg];
        if (*ptr == '-') {
            while (*(++ptr) != '\0') {
                switch (*ptr) {
                    case 'k':
                        stop_mounter();
                        exit(0);
                    case 'h':
                        usage();
                        exit(1);
                    case 'm':
                        flag_mount++;
                        break;
                    case 's':
                        start_mounter();
                        exit(0);
                    case 'u':
                        flag_unmount++;
                        break;
                    default:
                        printf("Invalid argument -%s\n", ptr);
                        usage();
                        exit(1);
                }
            }
        } else if (devname == NULL) {
            devname = ptr;
        } else if (unit_str == NULL) {
            unit_str = ptr;
            if ((sscanf(ptr, "%d%n", &unit_min, &pos) != 1) ||
                (ptr[pos] != '\0')) {
                printf("Invalid unit '%s'\n", ptr);
                exit(1);
            }
        } else if (lun_str == NULL) {
            lun_str = ptr;
            if ((sscanf(ptr, "%d%n", &lun_min, &pos) != 1) ||
                (ptr[pos] != '\0')) {
                printf("Invalid lun '%s'\n", ptr);
                exit(1);
            }
        } else if (flags_str == NULL) {
            flags_str = ptr;
            if ((sscanf(ptr, "%u%n", &open_flags, &pos) != 1) ||
                (ptr[pos] != '\0')) {
                printf("Invalid flags '%s'\n", ptr);
                exit(1);
            }
        } else {
            printf("too many arguments: %s\n", ptr);
        }
    }
    if (devname == NULL) {
        printf("You must supply a device name\n");
        exit(1);
    }
    if (unit_min == -1)
        unit_min = 0;
    else
        unit_max = unit_min;

    if (lun_min == -1)
        lun_min = 0;
    else
        lun_max = lun_min;

    if (flag_mount)
        scan_flags = MOUNTER_SCAN_FLAG_MOUNT;
    else if (flag_unmount)
        scan_flags = MOUNTER_SCAN_FLAG_UNMOUNT;
    status = notify_mounter(MOUNTER_CMD_SCAN, unit_min, unit_max,
                            lun_min, lun_max, scan_flags, devname, open_flags);
    printf("Mounter: %d\n", status);
    exit(0);
}
