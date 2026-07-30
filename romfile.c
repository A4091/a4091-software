//
// Copyright 2022-2026 Stefan Reinauer
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//

#ifdef DEBUG_DEVICE
#define USE_SERIAL_OUTPUT
#endif
#include "port.h"
#include "printf.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "device.h"
#include "attach.h"
#include "reloc.h"
#include <exec/resident.h>
#include <resources/filesysres.h>
#if HAVE_ROM
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/execbase.h>
#include <exec/lists.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#endif
#include "version.h"
#include "romfile.h"

#define DOSTYPE_CD01 0x43443031
#define DOSTYPE_CDVD 0x43445644

#if HAVE_ROM
static void track_rom_cd_filesystem(struct FileSysEntry *newROMEntry,
				    struct ExecBase *SysBase);
#endif

static int add_fs_from_kickstart(void)
{
    struct Resident *r = NULL;

    printf("CDFS in Kickstart... ");
    r=FindResident("cdfs");
    printf("%sfound.\n", r?"":"not ");

    if (r != NULL) {
        if (r && r->rt_Init) {
            printf("Initializing CDFS @%p... ", r);
            InitResident(r, 0);
            printf("done.\n");
            return 1;
        } else
            printf("No rt_Init.\n");
    }
    return 0;
}

static struct FileSysEntry *find_registered_filesystem(ULONG id1, ULONG id2)
{
    struct FileSysResource *FileSysResBase;
    struct FileSysEntry *fse;
    struct FileSysEntry *found = NULL;

    Forbid();
    FileSysResBase = (struct FileSysResource *)OpenResource(FSRNAME);
    if (FileSysResBase) {
        for (fse = (struct FileSysEntry *)
                    FileSysResBase->fsr_FileSysEntries.lh_Head;
             fse->fse_Node.ln_Succ;
             fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ) {
            if ((id1 && fse->fse_DosType == id1) ||
                (id2 && fse->fse_DosType == id2)) {
                found = fse;
                break;
            }
        }
    }
    Permit();
    return found;
}

#if HAVE_ROM
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

    /* If no end-of-rom signature is found below, all slots stay empty */
    memset(rom, 0, sizeof(*rom));

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
                if (rom->romfile_dostype[i] == 0)
                    printf("            no DosType: FS can not be "
                           "demand-loaded (romtool -T missing?)\n");
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

static int add_romfilesystem(romfiles_t *rom, int slot,
                             struct FileSysEntry **newEntry)
{
    uint32_t fs_seglist = 0;
    struct Resident *r = NULL;
    struct FileSysResource *FileSysResBase;
    struct FileSysEntry *fse = NULL;
    struct FileSysEntry *created = NULL;

    unsigned int i;
    if (newEntry)
        *newEntry = NULL;
    printf("Looking for FS in A4091 ROM slot %d... ", slot);

    if (rom->romfile_len[slot])
        fs_seglist = relocate(rom->romfile[slot], (uint32_t)asave->as_addr);

    printf("%sfound.\n", fs_seglist?"":"not ");

    if (fs_seglist == 0)
        return 0;

    printf("Resident struct... ");
    for (i=fs_seglist; i<fs_seglist + rom->romfile_len[slot]; i+=2) {
        if(*(uint16_t *)i == 0x4afc) {
            r = (struct Resident *)i;
            break;
        }
    }

    printf("%sfound.\n", r?"":"not ");

    if (r != NULL) {
        if (r && r->rt_Init) {
            printf("Initializing FS @%p... ", r);
            InitResident(r, fs_seglist >> 2);
            printf("done.\n");
            created = find_registered_filesystem(
                        rom->romfile_dostype[slot], 0);
            if (created &&
                created->fse_SegList != (BPTR)(fs_seglist >> 2))
                created = NULL;
            if (newEntry)
                *newEntry = created;
	    return 1;
        } else
            printf("No rt_Init.\n");
    }

    Forbid();
    FileSysResBase = (struct FileSysResource *)OpenResource(FSRNAME);

    for (fse = (struct FileSysEntry *)FileSysResBase->fsr_FileSysEntries.lh_Head;
	      fse->fse_Node.ln_Succ;
	      fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ) {
	if (fse->fse_DosType == rom->romfile_dostype[slot]) {
		printf("DosType already present. Skipping.\n");
		FileSysResBase = NULL;
	}
    }

    if (FileSysResBase) {

        fse = AllocMem(sizeof(struct FileSysEntry), MEMF_PUBLIC | MEMF_CLEAR);
        if (fse) {
            fse->fse_Node.ln_Name = (UBYTE*)device_id_string;
            fse->fse_DosType = rom->romfile_dostype[slot];
            fse->fse_Version = ((LONG)DEVICE_VERSION) << 16 | DEVICE_REVISION;
            fse->fse_PatchFlags = 0x190; // StackSize, SegList and GlobalVec
            fse->fse_SegList = fs_seglist >> 2;
            fse->fse_GlobalVec = -1;
            //fse->fse_StackSize = 5120;
            fse->fse_StackSize = 16384; // Is there a right answer here?
            fse->fse_Priority = 10;

            AddHead(&FileSysResBase->fsr_FileSysEntries,&fse->fse_Node);
            created = fse;
	}
    }
    Permit();
    if (newEntry)
        *newEntry = created;
    return (fse!=NULL);
}
#endif

/*
 * LoadFileSys() is called by the mounter whenever it needs a
 * filesystem for one of the given DosTypes. If a matching
 * FileSysEntry is already registered in FileSystem.resource, it does
 * nothing. Otherwise, filesystems are loaded from the A4091 ROM (or
 * initialized from Kickstart) on demand, i.e. when a partition or
 * medium actually requires them: the mounter only asks for CD01/CDVD
 * when it finds a data CD (and CDROM boot is enabled), and only asks
 * for other DosTypes when it finds a partition of that type. Returns
 * nonzero if a filesystem was initialized; the caller scans
 * FileSystem.resource for the result, since a filesystem with a
 * romtag (e.g. ODFileSystem) registers its own FileSysEntry inside
 * InitResident().
 */
LONG LoadFileSys(ULONG id1, ULONG id2)
{
	static int kickstart_tried;
	LONG loaded = 0;

	/* Already available? Nothing to do. */
	if (find_registered_filesystem(id1, id2))
		return 0;

	printf("Mounter requests filesystem %08lx/%08lx\n", id1, id2);

	/* A Kickstart-resident CDFS can only serve CD DosTypes. Only try
	 * once: InitResident() must not run again on a later request. */
	if (!kickstart_tried &&
	    (id1 == DOSTYPE_CD01 || id1 == DOSTYPE_CDVD ||
	     id2 == DOSTYPE_CD01 || id2 == DOSTYPE_CDVD)) {
		kickstart_tried = 1;
		loaded |= add_fs_from_kickstart();
	}

#if HAVE_ROM
	static romfiles_t rom;
	static int rom_parsed;
	static int slot_tried[3];
	int slot;

	if (!rom_parsed) {
		parse_romfiles(&rom);
		rom_parsed = 1;
	}

	for (slot = 1; slot < 3; slot++) {
		struct FileSysEntry *created = NULL;

		if (slot_tried[slot] ||
		    rom.romfile_len[slot] == 0 ||
		    rom.romfile_dostype[slot] == 0)
			continue;
		if (rom.romfile_dostype[slot] == id1 ||
		    rom.romfile_dostype[slot] == id2) {
			slot_tried[slot] = 1;
			loaded |= add_romfilesystem(&rom, slot, &created);
			if (created)
				track_rom_cd_filesystem(created, SysBase);
		}
	}
#endif
	return loaded;
}

#if HAVE_ROM

#define ROM_CD_MAX 16
#define RESMODULE_NEXT 0x80000000UL

static BOOL trackingIncomplete;
static struct FileSysEntry *romCDFSE;
static ULONG afterDOSModules[2];
static APTR previousResModules;
static BOOL afterDOSRegistered;
static BOOL afterDOSDone;

static APTR cd_cleanup_after_dos(BPTR segList asm("a0"),
				 struct ExecBase *SysBase asm("a6"));

static const char afterDOSName[] = XSTR(DEVNAME) ".cdcleanup";
static const char afterDOSId[] = XSTR(DEVNAME) " CD cleanup";

static const struct Resident afterDOSResident = {
	.rt_MatchWord = RTC_MATCHWORD,
	.rt_MatchTag = (struct Resident *)&afterDOSResident,
	.rt_EndSkip = (APTR)(&afterDOSResident + 1),
	.rt_Flags = RTF_AFTERDOS,
	.rt_Version = 0,
	.rt_Type = NT_UNKNOWN,
	.rt_Pri = -100,
	.rt_Name = (char *)afterDOSName,
	.rt_IdString = (char *)afterDOSId,
	.rt_Init = (APTR)cd_cleanup_after_dos
};

static BOOL is_cd_type(ULONG dosType)
{
	return dosType == DOSTYPE_CD01 || dosType == DOSTYPE_CDVD;
}

static void install_after_dos(struct ExecBase *SysBase)
{
	if (afterDOSRegistered)
		return;

	/*
	 * InitCode() does not re-sort a live ResModules array. Put this
	 * one-shot callback at its head so it runs before ramlib.
	 */
	Forbid();
	previousResModules = SysBase->ResModules;
	afterDOSModules[0] = (ULONG)&afterDOSResident;
	if (previousResModules)
		afterDOSModules[1] =
		    (ULONG)previousResModules | RESMODULE_NEXT;
	else
		afterDOSModules[1] = 0;
	SysBase->ResModules = afterDOSModules;
	afterDOSRegistered = TRUE;
	Permit();
}

static void track_rom_cd_filesystem(struct FileSysEntry *newROMEntry,
				    struct ExecBase *SysBase)
{
	if (!newROMEntry || !SysBase ||
	    SysBase->LibNode.lib_Version < 39 ||
	    !is_cd_type(newROMEntry->fse_DosType))
		return;

	if (romCDFSE && romCDFSE != newROMEntry) {
		trackingIncomplete = TRUE;
		return;
	}

	romCDFSE = newROMEntry;
	install_after_dos(SysBase);
}

/*
 * DOS copies fse_SegList into every DeviceNode which uses that handler.
 * Locate the ROM users after DOS has started them instead of requiring
 * the mounter to report each node as it is created.
 */
static LONG collect_rom_nodes(struct MsgPort *bootTask,
			      struct DeviceNode **nodes, UWORD *nodeCount,
			      struct MsgPort **tasks, UWORD *taskCount,
			      struct DosLibrary *DOSBase)
{
	struct DosList *entry;
	LONG result = 0;
	ULONG flags = LDF_DEVICES | LDF_READ;

	*nodeCount = 0;
	*taskCount = 0;
	entry = LockDosList(flags);
	if (!entry)
		return -1;

	while ((entry = NextDosEntry(entry, LDF_DEVICES))) {
		struct DeviceNode *deviceNode = (struct DeviceNode *)entry;

		if (deviceNode->dn_SegList != romCDFSE->fse_SegList)
			continue;
		if (deviceNode->dn_Task == bootTask)
			result = 1;
		if (*nodeCount >= ROM_CD_MAX) {
			result = -1;
			break;
		}
		nodes[(*nodeCount)++] = deviceNode;

		if (deviceNode->dn_Task) {
			UWORD i;

			for (i = 0; i < *taskCount; i++) {
				if (tasks[i] == deviceNode->dn_Task)
					break;
			}
			if (i == *taskCount)
				tasks[(*taskCount)++] = deviceNode->dn_Task;
		}
	}
	UnLockDosList(flags);
	return result;
}

static LONG rom_nodes_running(struct DosLibrary *DOSBase)
{
	struct DosList *entry;
	LONG result = 0;
	ULONG flags = LDF_DEVICES | LDF_READ;

	entry = LockDosList(flags);
	if (!entry)
		return -1;

	while ((entry = NextDosEntry(entry, LDF_DEVICES))) {
		struct DeviceNode *deviceNode = (struct DeviceNode *)entry;

		if (deviceNode->dn_SegList == romCDFSE->fse_SegList &&
		    deviceNode->dn_Task) {
			result = 1;
			break;
		}
	}
	UnLockDosList(flags);
	return result;
}

static BOOL remove_boot_node(struct DeviceNode *deviceNode,
			     struct ExpansionBase *ExpansionBase,
			     struct ExecBase *SysBase)
{
	struct BootNode *bootNode;
	struct BootNode *next;
	BOOL removed = FALSE;

	for (bootNode = (struct BootNode *)ExpansionBase->MountList.lh_Head;
	     bootNode->bn_Node.ln_Succ;
	     bootNode = next) {
		next = (struct BootNode *)bootNode->bn_Node.ln_Succ;
		if (bootNode->bn_DeviceNode == deviceNode) {
			Remove(&bootNode->bn_Node);
			removed = TRUE;
		}
	}
	return removed;
}

static void unlink_owned_fse(struct ExecBase *SysBase)
{
	struct FileSysResource *FileSysResBase;
	struct FileSysEntry *fse;
	BOOL found = FALSE;

	FileSysResBase = (struct FileSysResource *)OpenResource(FSRNAME);
	if (!FileSysResBase)
		return;

	Forbid();
	for (fse = (struct FileSysEntry *)
		     FileSysResBase->fsr_FileSysEntries.lh_Head;
	     fse->fse_Node.ln_Succ;
	     fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ) {
		if (fse == romCDFSE) {
			Remove(&fse->fse_Node);
			found = TRUE;
			break;
		}
	}
	Permit();

	if (found)
		printf("Removed ROM CD filesystem from FileSystem.resource\n");
	else
		printf("ROM CD filesystem was already unregistered\n");

	/*
	 * The entry and relocated handler remain allocated until reboot.
	 * This avoids invalidating pointers into the handler image.
	 */
	romCDFSE = NULL;
}

static APTR cd_cleanup_after_dos(BPTR segList asm("a0"),
				 struct ExecBase *SysBase asm("a6"))
{
	struct DosLibrary *DOSBase;
	struct ExpansionBase *ExpansionBase = NULL;
	struct MsgPort *bootTask;
	struct MsgPort *tasks[ROM_CD_MAX];
	struct DeviceNode *originalNodes[ROM_CD_MAX];
	struct DeviceNode *remainingNodes[ROM_CD_MAX];
	struct DosList *entry;
	struct DosList *list;
	BOOL cleanupOK = TRUE;
	LONG romUse;
	LONG running;
	UWORD originalCount;
	UWORD remainingCount;
	UWORD taskCount;
	ULONG lockFlags = LDF_DEVICES | LDF_WRITE;

	(void)segList;

	Forbid();
	if (SysBase->ResModules == afterDOSModules)
		SysBase->ResModules = previousResModules;
	Permit();

	if (afterDOSDone)
		return NULL;
	afterDOSDone = TRUE;

	if (!romCDFSE)
		return NULL;

	DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
	if (!DOSBase)
		return NULL;

	bootTask = DOSBase->dl_Root ? DOSBase->dl_Root->rn_BootProc : NULL;
	if (!bootTask) {
		printf("Keeping ROM CD filesystem: boot state is incomplete\n");
		CloseLibrary(&DOSBase->dl_lib);
		return NULL;
	}

	romUse = collect_rom_nodes(bootTask, originalNodes, &originalCount,
				   tasks, &taskCount, DOSBase);
	if (romUse > 0) {
		printf("Keeping ROM CD filesystem for boot handler\n");
		CloseLibrary(&DOSBase->dl_lib);
		return NULL;
	}
	if (romUse < 0 || trackingIncomplete) {
		printf("Keeping ROM CD filesystem: cleanup state is incomplete\n");
		CloseLibrary(&DOSBase->dl_lib);
		return NULL;
	}

	if (originalCount == 0) {
		unlink_owned_fse(SysBase);
		CloseLibrary(&DOSBase->dl_lib);
		return NULL;
	}

	ExpansionBase = (struct ExpansionBase *)
				OpenLibrary("expansion.library", 34);
	if (!ExpansionBase) {
		printf("Keeping ROM CD filesystem: expansion unavailable\n");
		CloseLibrary(&DOSBase->dl_lib);
		return NULL;
	}

	for (UWORD i = 0; i < taskCount; i++) {
		if (!DoPkt0(tasks[i], ACTION_DIE))
			printf("ROM CD handler rejected ACTION_DIE\n");
	}

	running = rom_nodes_running(DOSBase);
	for (UWORD retry = 0; running > 0 && retry < 10; retry++) {
		Delay(1);
		running = rom_nodes_running(DOSBase);
	}
	if (running != 0) {
		printf("Keeping ROM CD filesystem: handler did not exit\n");
		CloseLibrary(&ExpansionBase->LibNode);
		CloseLibrary(&DOSBase->dl_lib);
		return NULL;
	}

	list = LockDosList(lockFlags);
	if (!list) {
		printf("Keeping ROM CD filesystem: DosList lock failed\n");
		CloseLibrary(&ExpansionBase->LibNode);
		CloseLibrary(&DOSBase->dl_lib);
		return NULL;
	}

	remainingCount = 0;
	entry = list;
	while ((entry = NextDosEntry(entry, LDF_DEVICES))) {
		struct DeviceNode *deviceNode = (struct DeviceNode *)entry;

		if (deviceNode->dn_SegList != romCDFSE->fse_SegList)
			continue;
		if (deviceNode->dn_Task || remainingCount >= ROM_CD_MAX) {
			cleanupOK = FALSE;
			break;
		}
		remainingNodes[remainingCount++] = deviceNode;
	}

	if (cleanupOK) {
		for (UWORD i = 0; i < remainingCount; i++) {
			struct DeviceNode *deviceNode = remainingNodes[i];

			if (!RemDosEntry((struct DosList *)deviceNode)) {
				printf("Could not remove unused CD device\n");
				cleanupOK = FALSE;
				continue;
			}
			printf("Removed unused CD device %s\n",
			       (UBYTE *)BADDR(deviceNode->dn_Name) + 1);
		}
	}
	UnLockDosList(lockFlags);

	if (cleanupOK) {
		for (UWORD i = 0; i < originalCount; i++) {
			BOOL removed;

			Forbid();
			removed = remove_boot_node(originalNodes[i],
						   ExpansionBase, SysBase);
			Permit();
			if (!removed)
				printf("No expansion boot node for CD device %p\n",
				       originalNodes[i]);
		}
	}

	CloseLibrary(&ExpansionBase->LibNode);

	if (cleanupOK)
		unlink_owned_fse(SysBase);

	CloseLibrary(&DOSBase->dl_lib);
	return NULL;
}

#endif
