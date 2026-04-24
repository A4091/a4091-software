// SPDX-License-Identifier: GPL-2.0-only
/* This file is part of a4092flash
 * Copyright (C) 2023 Matthew Harlum <matt@harlum.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <proto/alib.h>
#include <dos/dosextens.h>

#include "../../a4091.h"
#include "flash.h"
#include "gui.h"
#include "hardware.h"
#include "main.h"
#include "config.h"
#include "nvram_flash.h"

#define A4091_ROM_MAGIC1 0xFFFF5352
#define A4091_ROM_MAGIC2 0x2F434448
#define MAX_ROM_INSPECT_SIZE (64 * 1024)

const char ver[] = VERSION_STRING;

/* The GUI path has a large stack frame after GadTools/status/progress support.
 * Ask clib2 to relaunch with enough stack instead of depending on the shell's
 * current Stack setting.
 */
unsigned int __stack_size = 32768;

struct Library *DosBase;
#if __GNUC__ < 11
struct ExecBase *SysBase;
#endif
struct ExpansionBase *ExpansionBase = NULL;
struct Config *config;
struct nvramParams nvramParams;
#ifdef SHARED_REGISTERS
bool devsInhibited = false;
#endif

/**
 * _ColdReboot()
 *
 * Kickstart V36 (2.0+) and up contain a function for this
 * But for 1.3 we will need to provide our own function
 */
static void _ColdReboot(void)
{
  // Copied from coldboot.asm
  // http://amigadev.elowar.com/read/ADCD_2.1/Hardware_Manual_guide/node02E3.html
  asm("move.l  4,a6               \n\t" // SysBase
      "lea.l   DoIt(pc),a5        \n\t"
      "jsr     -0x1e(a6)          \n\t" // Call from Supervisor mode
      ".align 4                   \n\t" // Must be aligned!
      "DoIt:                      \n\t"
      "lea.l   0x1000000,a0       \n\t" // (ROM end)
      "sub.l   -0x14(a0),a0       \n\t" // (ROM end)-(ROM Size)
      "move.l  4(a0),a0           \n\t" // Initial PC
      "subq.l  #2,(a0)            \n\t" // Points to second RESET
      "reset                      \n\t"
      "jmp     (a0)");
}

#if SHARED_REGISTERS
/**
 * inhibitDosDevs
 * 
 * inhibit/uninhibit all drives
 * Send an ACTION_INHIBIT packet to all devices to flush the buffers to disk first
 * 
 * @param inhibit (bool) True: inhibit, False: uninhibit 
 */
bool inhibitDosDevs(bool inhibit)
{
  bool success = true;
  struct MsgPort *mp = CreatePort(NULL,0);
  struct Message msg;
  struct DosPacket __aligned packet;
  struct DosList *dl;
  struct dosDev *dd;

  struct MinList devs;
  NewList((struct List *)&devs);

  if (mp) {
    packet.dp_Port = mp;
    packet.dp_Link = &msg;
    msg.mn_Node.ln_Name = (char *)&packet;

    if (SysBase->SoftVer >= 36) {

      dl = LockDosList(LDF_DEVICES|LDF_READ);
      // Build a list of dos devices to inhibit
      // We need to send a packet to the FS to do the inhibit after releasing the lock
      // So build a list of devs to be (un)-inhibited
      while ((dl = NextDosEntry(dl,LDF_DEVICES))) {
        dd = AllocMem(sizeof(struct dosDev),MEMF_ANY|MEMF_CLEAR);
        if (dd) {
          if (dl->dol_Task) { // Device has a FS process?
            dd->handler = dl->dol_Task;
            AddTail((struct List *)&devs,(struct Node *)dd);
          }
        }
      }
      UnLockDosList(LDF_DEVICES|LDF_READ);

    } else {
      // For Kickstart 1.3
      // Build a list of dos devices the old fashioned way
      struct RootNode *rn = DOSBase->dl_Root;
      struct DosInfo *di = BADDR(rn->rn_Info);

      Forbid();
      // Build a list of dos devices to inhibit
      // We need to send a packet to the FS but that can't be done while in Forbid()
      // So build a list of devs to be (un)-inhibited
      for (dl = BADDR(di->di_DevInfo); dl; dl = BADDR(dl->dol_Next)) {
        if (dl->dol_Type == DLT_DEVICE && dl->dol_Task) {
          dd = AllocMem(sizeof(struct dosDev),MEMF_ANY|MEMF_CLEAR);
          if (dd) {
            if (dl->dol_Task) { // Device has a FS process?
              dd->handler = dl->dol_Task;
              AddTail((struct List *)&devs,(struct Node *)dd);
            }
          }
        }
      }
      Permit();
    }

    struct dosDev *next = NULL;
    // Send an ACTION_INHIBIT packet directly to the FS
    for (dd = (struct dosDev *)devs.mlh_Head; dd->mn.mln_Succ; dd = next) {
      if (inhibit) {
        packet.dp_Port = mp;
        packet.dp_Type = ACTION_FLUSH;
        PutMsg(dd->handler,&msg);
        WaitPort(mp);
        GetMsg(mp);
      }

      for (int t=0; t < 3; t++) {
        packet.dp_Port = mp;
        packet.dp_Type = ACTION_INHIBIT;
        packet.dp_Arg1 = (inhibit) ? DOSTRUE : DOSFALSE;
        PutMsg(dd->handler,&msg);
        WaitPort(mp);
        GetMsg(mp);

        if (packet.dp_Res1 == DOSTRUE || packet.dp_Res2 == ERROR_ACTION_NOT_KNOWN)
          break;

        Delay(1*TICKS_PER_SECOND);
      }

      if (packet.dp_Res1 == DOSFALSE && packet.dp_Res2 != ERROR_ACTION_NOT_KNOWN) {
        success = false;
      }

      next = (struct dosDev *)dd->mn.mln_Succ;
      Remove((struct Node *)dd);
      FreeMem(dd,sizeof(struct dosDev));

    }

    DeletePort(mp);

  } else {
    success = false;
  }

  return success;
}
#endif

/**
 * promptUser
 *
 * Ask if the user wants to update this board
 * @param config pointer to the config struct
 * @return boolean true / false
 */
static bool promptUser(struct Config *config)
{
  int c;
  char answer = 'y'; // Default to yes

  printf("Update this device? (Y)es/(n)o/(a)ll: ");

  if (config->assumeYes) {
    printf("y\n");
    return true;
  }

  while ((c = getchar()) != '\n' && c != EOF) answer = c;

  answer |= 0x20; // convert to lowercase;

  if (answer == 'a') {
    config->assumeYes = true;
    return true;
  }

  return (answer == 'y');
}

static BOOL probeFlash(ULONG romSize);
static BOOL parse_nvram_params(struct Config* config);
static void execute_nvram_operations(struct scsiBoard* board);

static BOOL cli_requests_gui(int argc, char *argv[])
{
  int i;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--gui") == 0 || strcmp(argv[i], "-G") == 0)
      return TRUE;
  }

  return FALSE;
}

static BOOL started_from_workbench(void)
{
  struct Process *process = (struct Process *)FindTask(NULL);

  return process && process->pr_CLI == 0;
}

int main(int argc, char *argv[])
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
  SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop
  DosBase = OpenLibrary("dos.library",0);

  int rc = 0;
  int boards_found = 0;
  int a4092_found = 0;
  int board_errors = 0;

  void *driver_buffer = NULL;

  ULONG romSize    = 0;

  if (DosBase == NULL) {
    return(20);
  }

  if (argc == 0 || started_from_workbench()) {
    rc = run_workbench_ui();
    goto exit;
  }

  if (cli_requests_gui(argc, argv)) {
    rc = run_workbench_ui();
    goto exit;
  }

  printf("\n%s\n\n", VERSION);

  struct Task *task = FindTask(0);
  SetTaskPri(task,20);
  if ((config = configure(argc,argv)) != NULL) {

    if (config->nvramFlash) {
      if (!parse_nvram_params(config)) {
        rc = 5;
        goto exit;
      }
    }

    if (config->writeFlash && config->scsi_rom_filename) {
      romSize = getFileSize(config->scsi_rom_filename);
      if (romSize == 0) {
        rc = 5;
        goto exit;
      }

      if (romSize > 2048*1024) {
        printf("ROM file too large.\n");
        rc = 5;
        goto exit;
      }

      if (romSize < 32*1024) {
        printf("ROM file too small.\n");
        rc = 5;
        goto exit;
      }

      driver_buffer  = AllocMem(romSize,MEMF_ANY|MEMF_CLEAR);

      if (driver_buffer) {
        if (readFileToBuf(config->scsi_rom_filename,driver_buffer) == false) {
          rc = 5;
          goto exit;
        }
      } else {
        printf("Couldn't allocate memory.\n");
        rc = 5;
        goto exit;
      }
    }
   
#if SHARED_REGISTERS
    if (!inhibitDosDevs(true)) {
      printf("Failed to inhibit AmigaDOS volumes, wait for disk activity to stop and try again.\n");
      rc = 5;
      inhibitDosDevs(false);
      goto exit;
    };

    devsInhibited = true;
#endif

    if ((ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library",0)) != NULL) {

      struct ConfigDev *cd = NULL;
      struct scsiBoard board;

      while ((cd = FindConfigDev(cd,-1,-1)) != NULL) {
        bool legacy_id;
        enum boardType detected_card;
        UBYTE manufId = 0;
        UBYTE devId = 0;
        ULONG sectorSize = 0;
        ULONG flashSize = 0;
        const char *board_name;

        board.cd = cd;
        detected_card = board_type_from_configdev(cd);
        legacy_id = (detected_card == BOARD_A4091);
        if (detected_card == BOARD_NONE)
          continue;
        boards_found++;
        setup_a4092_board(&board);

        if (!flash_init(&manufId, &devId, board.flashbase, &flashSize, &sectorSize)) {
          if (legacy_id) {
            if (manufId == 0x9f && devId == 0xaf) {
              printf("Found A4091 at Address 0x%06x\n", (int)cd->cd_BoardAddr);
              printf("a4092flash does not work on A4091.\n");
            } else {
              printf("Found legacy-ID board at Address 0x%06x\n",
                     (int)cd->cd_BoardAddr);
              printf("No supported flash detected (Manufacturer: %02X Device: %02X)\n",
                     manufId, devId);
              board_errors = 1;
            }
          } else {
            printf("Error: %s flash not detected (Manufacturer: %02X Device: %02X)\n",
                   board_type_name(detected_card), manufId, devId);
            board_errors = 1;
          }
          continue;
        }

        if (legacy_id && detected_card == BOARD_A4091)
          detected_card = BOARD_A4092;

        board_name = board_type_name(detected_card);
        a4092_found++;
        if (legacy_id && detected_card == BOARD_A4092)
          printf("Found legacy-ID %s at Address 0x%06x\n",
                 board_name, (int)cd->cd_BoardAddr);
        else
          printf("Found %s at Address 0x%06x\n",
                 board_name, (int)cd->cd_BoardAddr);

        {
          // Display version information before prompting
          if (config->writeFlash && driver_buffer) {
            struct romInfo flash_info;
            struct romInfo file_info;
            ULONG inspect_size = (flashSize < MAX_ROM_INSPECT_SIZE) ? flashSize : MAX_ROM_INSPECT_SIZE;
            char *flash_buf = AllocMem(inspect_size, 0);

            if (flash_buf) {
              flash_readBuf(0, (UBYTE *)flash_buf, inspect_size);
              summarize_rom_buffer((UBYTE *)flash_buf, inspect_size, &flash_info);

              printf("  Installed: ");
              printf("%s\n", flash_info.summary);
              FreeMem(flash_buf, inspect_size);
            }

            summarize_rom_buffer(driver_buffer, romSize, &file_info);
            printf("  Update to: ");
            printf("%s\n", file_info.summary);
          }

          // Ask the user if they wish to update this board
          if ((config->writeFlash || config->eraseFlash) && !promptUser(config)) continue;
          ULONG bankSize = 65536; // hardcode supported image size for now
          if (config->eraseFlash) {
            printf("Erasing whole flash.\n");
            if (!flash_erase_chip()) {
              fprintf(stderr, "ERROR: Flash erase/verify failed!\n");
              rc = 5;
              goto exit;
            }
          }

          if (config->writeFlash && config->scsi_rom_filename) {
            if (config->eraseFlash == false) {
              if (sectorSize > 0) {
                printf("Erasing flash bank.\n");
                if (!flash_erase_bank(0, sectorSize, bankSize)) {
                  fprintf(stderr, "ERROR: Flash bank erase/verify failed!\n");
                  rc = 5;
                  goto exit;
                }
              } else {
                printf("Erasing whole flash.\n");
                if (!flash_erase_chip()) {
                  fprintf(stderr, "ERROR: Flash erase/verify failed!\n");
                  rc = 5;
                  goto exit;
                }
              }
            }
            printf("Writing %s ROM image to flash memory.\n", board_name);
            writeBufToFlash(&board, driver_buffer, board.flashbase, romSize);
            printf("\n");
          }

	  if (config->readFlash && config->scsi_rom_filename && flashSize) {
            printf("Writing %s flash memory to file.\n", board_name);
            if (writeFlashToFile(config->scsi_rom_filename,flashSize) == false) {
              rc = 5;
              goto exit;
            }
	  }

	  if (config->probeFlash && flashSize) {
            if (probeFlash(flashSize) == false) {
              rc = 5;
              goto exit;
            }
	  }

	  if (config->nvramFlash) {
            execute_nvram_operations(&board);
	  }
        }
      }

      if (boards_found == 0) {
        printf("No supported board(s) found\n");
        rc = 5;
      } else if (a4092_found == 0) {
        rc = 5;
      } else if (board_errors) {
        rc = 5;
      }
    } else {
      printf("Couldn't open Expansion.library.\n");
      rc = 5;
    }

    if (config->rebootRequired) {
      printf("Press return to reboot.\n");
      getchar();
      if (SysBase->SoftVer >= 36) {
        ColdReboot();
      } else {
        _ColdReboot();
      }
    }

#if SHARED_REGISTERS
    if (devsInhibited)
      inhibitDosDevs(false);
#endif

  } else {
    usage();
  }

exit:
  flash_cleanup();
  if (driver_buffer)  FreeMem(driver_buffer,romSize);
  if (config)         FreeMem(config,sizeof(struct Config));
  if (ExpansionBase)  CloseLibrary((struct Library *)ExpansionBase);
  if (DosBase)        CloseLibrary((struct Library *)DosBase);
  return (rc);
}

/**
 * getFileSize
 *
 * @brief return the size of a file in bytes
 * @param filename file to check the size of
 * @returns File size in bytes
*/
ULONG getFileSize(char *filename)
{
  BPTR fileLock;
  ULONG fileSize = 0;
  struct FileInfoBlock *FIB;

  FIB = (struct FileInfoBlock *)AllocMem(sizeof(struct FileInfoBlock),MEMF_CLEAR);

  if ((fileLock = Lock(filename,ACCESS_READ)) != 0) {

    if (Examine(fileLock,FIB)) {
      fileSize = FIB->fib_Size;
    }

  } else {
    printf("Error opening %s\n",filename);
    fileSize = 0;
  }

  if (fileLock) UnLock(fileLock);
  if (FIB) FreeMem(FIB,sizeof(struct FileInfoBlock));

  return (fileSize);
}

/**
 * readFileToBuF
 *
 * @brief Read the rom file to a buffer
 * @param filename Name of the file to open
 * @return true on success
*/
BOOL readFileToBuf(char *filename, void *buffer)
{
  ULONG romSize = getFileSize(filename);
  BOOL ret = true;

  if (romSize == 0) return false;

  BPTR fh;

  if (buffer) {
    fh = Open(filename,MODE_OLDFILE);

    if (fh) {
      Read(fh,buffer,romSize);
      Close(fh);
    } else {
      printf("Error opening %s\n",filename);
      return false;
    }

  } else {
    return false;
  }

  return ret;
}

/**
 * writeFlashToFile()
 *
 * Write the Flash content to the specified file
 *
 * @param filename file to write
 * @param size number of bytes to write
 * @returns true on success
*/
BOOL writeFlashToFile(char *filename, ULONG romSize)
{
  BOOL ret = true;
  char * buffer;

  if (romSize == 0) return false;
  fprintf (stdout, "Flash size: %d KB\n", (unsigned int)(romSize / 1024));
  buffer = AllocMem(romSize, 0);

  BPTR fh;

  if (buffer) {
    fprintf(stdout, "Reading Flash...\n");
    int i;
    for (i=0; i<romSize; i++) {
      buffer[i] = flash_readByte(i);
    }
    fprintf(stdout, "Writing File %s...\n", filename);
    fh = Open(filename,MODE_NEWFILE);

    if (fh) {
      Write(fh,buffer,romSize);
      Close(fh);
      FreeMem(buffer, romSize);
    } else {
      printf("Error opening %s\n",filename);
      FreeMem(buffer, romSize);
      return false;
    }

  } else {
    return false;
  }

  return ret;
}

/**
 * writeBufToFlash()
 *
 * Write the buffer to the currently selected flash bank
 *
 * @param source pointer to the source data
 * @param dest pointer to the flash base
 * @param size number of bytes to write
 * @returns true on success
*/
static void emit_write_progress(flashWriteProgressFn progressFn, void *progressCtx,
                                enum flashWritePhase phase, ULONG done,
                                ULONG total, int *lastProgress)
{
  int progress;

  if (!progressFn || total == 0)
    return;

  progress = (int)((done * 100UL) / total);
  if (progress > 100)
    progress = 100;

  if (*lastProgress != progress) {
    progressFn(progressCtx, phase, done, total);
    *lastProgress = progress;
  }
}

BOOL writeBufToFlashWithProgress(struct scsiBoard *board, UBYTE *source,
                                 volatile UBYTE *dest, ULONG size,
                                 flashWriteProgressFn progressFn,
                                 void *progressCtx)
{
  UBYTE destVal   = 0;
  bool showConsole = (progressFn == NULL) && !flash_status_sink_active();
  int lastWriteProgress = -1;
  int lastVerifyProgress = -1;

  (void)board;
  (void)dest;

  if (showConsole) {
    fprintf(stdout,"Writing:     ");
    fflush(stdout);
  } else {
    flash_printf("Writing flash...\n");
  }

  for (ULONG i = 0; i < size; i++) {
    if (showConsole) {
      int progress = (size > 1) ? (int)((i * 100UL) / (size - 1)) : 100;

      if (lastWriteProgress != progress) {
        fprintf(stdout,"\b\b\b\b%3d%%",progress);
        fflush(stdout);
        lastWriteProgress = progress;
      }
    }

    flash_writeByte(i, source[i]);

    if (!showConsole) {
      emit_write_progress(progressFn, progressCtx, FLASH_WRITE_PHASE_PROGRAM,
                          i + 1, size, &lastWriteProgress);
    }

  }

  if (showConsole) {
    fprintf(stdout,"\n");
    fflush(stdout);

    fprintf(stdout,"Verifying:     ");
  } else {
    flash_printf("Verifying flash...\n");
  }

  for (ULONG i = 0; i < size; i++) {
    if (showConsole) {
      int progress = (size > 1) ? (int)((i * 100UL) / (size - 1)) : 100;

      if (lastVerifyProgress != progress) {
        fprintf(stdout,"\b\b\b\b%3d%%",progress);
        fflush(stdout);
        lastVerifyProgress = progress;
      }
    }

    destVal = flash_readByte(i);
    if (source[i] != destVal) {
          flash_printf("Verification failed at offset %06lx - Expected %02X but read %02X\n",
                              (unsigned long)i, source[i], destVal);
          return false;
    }

    if (!showConsole) {
      emit_write_progress(progressFn, progressCtx, FLASH_WRITE_PHASE_VERIFY,
                          i + 1, size, &lastVerifyProgress);
    }
  }

  if (showConsole) {
    fprintf(stdout,"\n");
    fflush(stdout);
  }

  return true;
}

BOOL writeBufToFlash(struct scsiBoard *board, UBYTE *source, volatile UBYTE *dest,
                     ULONG size)
{
  return writeBufToFlashWithProgress(board, source, dest, size, NULL, NULL);
}

/**
 * probeFlash()
 *
 * Analyze flash content
 *
 * @param romSize total flash size in bytes
 * @returns true on success
*/
static BOOL probeFlash(ULONG romSize)
{
  BOOL ret = true;
  const ULONG BANK_SIZE = 0x10000; // 64KB
  const ULONG SEARCH_SIZE = 4096;

  if (romSize == 0) return false;

  // Check for 64KB image first
  char magic_buf[8];
  for (int i = 0; i < 8; i++) {
    magic_buf[i] = flash_readByte(BANK_SIZE - 8 + i);
  }

  ULONG magic1, magic2;
  memcpy(&magic1, magic_buf, 4);
  memcpy(&magic2, magic_buf + 4, 4);

  if (magic1 == A4091_ROM_MAGIC1 && magic2 == A4091_ROM_MAGIC2) {
    printf("Found 64KB supported ROM image.\n");
  } else {
    // Check for 32KB image
    for (int i = 0; i < 8; i++) {
      magic_buf[i] = flash_readByte((BANK_SIZE / 2) - 8 + i);
    }

    memcpy(&magic1, magic_buf, 4);
    memcpy(&magic2, magic_buf + 4, 4);

    if (magic1 == A4091_ROM_MAGIC1 && magic2 == A4091_ROM_MAGIC2) {
      printf("Found 32KB supported ROM image.\n");
    } else {
      printf("Not a standard image.\n");
    }
  }

  ULONG search_size = (romSize < SEARCH_SIZE) ? romSize : SEARCH_SIZE;
  char *buffer = AllocMem(search_size, 0);
  if (buffer) {
    for (int i = 0; i < search_size; i++) {
      buffer[i] = flash_readByte(i);
    }

    struct romInfo info;
    summarize_rom_buffer((UBYTE *)buffer, search_size, &info);
    printf("Version: %s\n", info.summary);

    FreeMem(buffer, search_size);
  } else {
    ret = false;
  }

  return ret;
}

static BOOL parse_nvram_params(struct Config* config) {
    if (!config->nvramCommand) return FALSE;

    memset(&nvramParams, 0, sizeof(nvramParams));
    
    char *cmd_copy = AllocMem(strlen(config->nvramCommand) + 1, MEMF_ANY);
    if (!cmd_copy) {
        printf("Memory allocation failed\n");
        return FALSE;
    }
    strcpy(cmd_copy, config->nvramCommand);
    
    char *token = strtok(cmd_copy, ";");
    while (token != NULL) {
        char *equals_pos = strchr(token, '=');
        
        if (strcmp(token, "init") == 0) {
            nvramParams.need_init = true;
        }
        else if (strcmp(token, "osflags") == 0) {
            nvramParams.need_read = true;
            nvramParams.read_osflags = true;
        }
        else if (strcmp(token, "switchflags") == 0) {
            nvramParams.need_read = true;
            nvramParams.read_switchflags = true;
        }
        else if (equals_pos != NULL) {
            *equals_pos = '\0';
            char *value_str = equals_pos + 1;
            
            if (strcmp(token, "osflags") == 0) {
                unsigned int value;
                if (sscanf(value_str, "%x", &value) != 1 || value > 0xFF) {
                    printf("Invalid hex value for osflags: %s\n", value_str);
                    FreeMem(cmd_copy, strlen(config->nvramCommand) + 1);
                    return FALSE;
                }
                nvramParams.need_write = true;
                nvramParams.write_osflags = (uint8_t)value;
                nvramParams.write_osflags_set = true;
            }
            else if (strcmp(token, "switchflags") == 0) {
                unsigned int value;
                if (sscanf(value_str, "%x", &value) != 1 || value > 0xFF) {
                    printf("Invalid hex value for switchflags: %s\n", value_str);
                    FreeMem(cmd_copy, strlen(config->nvramCommand) + 1);
                    return FALSE;
                }
                nvramParams.need_write = true;
                nvramParams.write_switchflags = (uint8_t)value;
                nvramParams.write_switchflags_set = true;
            }
            else {
                printf("Unknown NVRAM command: %s\n", token);
                FreeMem(cmd_copy, strlen(config->nvramCommand) + 1);
                return FALSE;
            }
        }
        else {
            printf("Unknown NVRAM command: %s\n", token);
            FreeMem(cmd_copy, strlen(config->nvramCommand) + 1);
            return FALSE;
        }
        
        token = strtok(NULL, ";");
    }
    
    FreeMem(cmd_copy, strlen(config->nvramCommand) + 1);
    return TRUE;
}

static void execute_nvram_operations(struct scsiBoard* board) {
    if (nvramParams.need_init) {
        printf("Initializing NVRAM partition at offset 0x%x...\n", NVRAM_OFFSET);
        int result = flash_format_nvram_partition((uintptr_t)board->flashbase + NVRAM_OFFSET, NVRAM_SIZE);
        if (result == NVRAM_OK) {
            printf("NVRAM partition initialized successfully.\n");
        } else {
            printf("Failed to initialize NVRAM partition (error %d).\n", result);
            return;
        }
    }
    
    if (nvramParams.need_read) {
        struct nvram_t entry;
        int result = flash_read_nvram((uintptr_t)board->flashbase + NVRAM_OFFSET, &entry);
        
        if (result == NVRAM_ERR_BAD_MAGIC) {
            printf("No NVRAM partition. Please run a4092flash -F init\n");
            return;
        }
        if (result != NVRAM_OK) {
            printf("Error reading NVRAM: %d\n", result);
            return;
        }
        
        if (nvramParams.read_osflags) {
            printf("osflags=0x%02x\n", entry.settings.os_flags);
        }
        if (nvramParams.read_switchflags) {
            printf("switchflags=0x%02x\n", entry.settings.switch_flags);
        }
    }

    if (nvramParams.need_write) {
        struct nvram_t entry = {0};
        int read_result = flash_read_nvram((uintptr_t)board->flashbase + NVRAM_OFFSET, &entry);
        
        if (read_result == NVRAM_ERR_BAD_MAGIC && !nvramParams.need_init) {
            printf("No NVRAM partition. Please run a4092flash -F init\n");
            return;
        }
        
        if (read_result != NVRAM_OK && read_result != NVRAM_ERR_NO_ENTRIES && !nvramParams.need_init) {
            printf("Error reading NVRAM: %d\n", read_result);
            return;
        }
        
        if (nvramParams.write_osflags_set) {
            entry.settings.os_flags = nvramParams.write_osflags;
        }
        if (nvramParams.write_switchflags_set) {
            entry.settings.switch_flags = nvramParams.write_switchflags;
        }
        int write_result = flash_write_nvram((uintptr_t)board->flashbase + NVRAM_OFFSET, &entry);
        if (write_result == NVRAM_OK) {
            printf("NVRAM entry written successfully.\n");
        } else {
            printf("Failed to write NVRAM entry (error %d).\n", write_result);
        }
    }
}
