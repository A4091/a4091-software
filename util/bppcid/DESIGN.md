# Blizzard PPC ROM identification editor

## Purpose and limits

Test whether the SCSI identity byte affects a retrofitted Blizzard PPC.
Change only offset 0x10 between 'H' and 'I', preserving the remaining ID,
firmware, serial data and settings. This is an experiment, not an established
fix for the SCSI reset-release failure. No CPLD programming is performed.

## Evidence

- Supplied `ppc/HOW_TO_INSTALL_THE_FLASH.txt` selects BlizzPPC060Update for
  both SCSI and non-SCSI 060 boards; PPCUpdate is for CyberStorm PPC.
- BlizzPPC060Update .text+0x2cf2 saves ROM offsets 8..27, and .text+0x1906
  copies them into its replacement image. Thus an ordinary update preserves
  the original identity. .text+0x822 asserts SCSI reset, +0x828 writes 0x42 to
  0xf60092 to unlock flash, and +0x848 locks it through 0xf60093.
- WinUAE cpuboard.cpp confirms these byte accesses, the 384 KiB ROM mapping
  at 0xf00000 with SCSI held reset, and I/O starting at 0xf60000. Do not
  pretend a 512 KiB linear memory dump is a complete flash backup.
- Reference ROM bootstrap sums big-endian longs from offsets 0x20 through
  0x4ffff inclusive. Identity offset 0x10 is outside this checksum.
- [AMD Am29F040B datasheet](https://www.farnell.com/datasheets/11690.pdf):
  Am29F040/040B (JEDEC 01:a4) uses 64 KiB sectors. H (0x48) to I (0x49)
  needs erase because it changes a zero bit to one. Support only this ID and
  refuse protected sectors. The algorithm uses AA/55 unlock cycles at
  0x5555/0x2aaa, matching the supplied updater and WinUAE.

## Operation

1. Require Blizzard resident, board identifier and 68060. Refuse a loaded
   SCSI driver, PPC software, or released SCSI reset. Ask the operator to boot
   without startup-sequence. No force override.
   Use 68060 PLPAR/PLPAW to verify identity mapping and write access for every
   4 KiB page of mapped ROM and board I/O, in user and supervisor modes.
   Catch access/illegal-instruction exceptions and restore vectors and DFC.
   Repeat this check in the critical section immediately before flash access.
   This permits the firmware's normal MMU setup without accepting ROM shadows.
   Translation and access-error behavior follow the
   [MC68060 manual](https://www.nxp.com/docs/en/data-sheet/MC68060UM.pdf).
2. Read the mapped 384 KiB twice with caches disabled, validate the ID and
   bootstrap checksum. Default operation is read-only; a proposed edit is a
   dry run unless --write is supplied with --expect-id and --backup.
3. Create an exclusive backup, close it, reopen and compare every byte before
   issuing any flash commands. Never overwrite an existing backup.
4. Disable caches and interrupts, hold the PPC in reset (bit 0x10, not AUX
   bit 0x02), and verify that it stopped before entering flash command mode.
   All erase/program/poll/verify code and buffers are in 68k RAM. No OS calls
   or printing while flash is in command mode. Preserve SCSI reset.
5. Identify flash and sector protection, compare current ROM with the saved
   image again, erase sector zero only, program its modified RAM copy, then
   verify all mapped bytes. Poll DQ7/DQ5 with a finite read budget.
6. On destructive failure, attempt one restoration of the original sector
   and verify it. Always reset flash to read mode and lock writes. Restart
   the PPC only after a verified ROM; retain reset if recovery failed.
7. Report the actual outcome. Require a complete power-off before evaluating
   the changed identity using ncr7xx -R.

## Validation stages

- Host flash model: command sequencing, erase geometry, required 0-to-1
  change, byte preservation, unsupported/protected flash, timeouts,
  verification failures and rollback.
- Build the actual Amiga binary and inspect its RAM-only critical path.
- Disposable WinUAE ROM: dry run, guards, write and reverse edit, backups,
  persisted ROM byte comparison. WinUAE overwrites the serial on startup:
  a test-only guest fixture must seed H after boot; this is not physical
  proof that changing the ID enables SCSI. Emulator flash timing and faults
  are not a substitute for chip-level validation.
- Physical execution remains the user's experiment. Keep the original
  backup available for external-programmer recovery if power fails.

## Validation results (2026-10-19)

- Native build passes with warnings treated as errors. The flash engine's
  compiled call sites use its own RAM code and supplied RAM callbacks;
  there are no OS or C library calls during flash command mode.
- Host NOR model passes forward/reverse identity edits, sector boundaries,
  checksum and byte preservation, unknown/protected chip refusals, stale
  snapshot and invalid-plan refusals, erase/program/DQ5/timeout/verification
  failures, successful restoration, and bounded failed restoration.
- WinUAE with actual 68060 MMU emulation accepts the firmware's normal
  identity mapping. A test-only variant temporarily installs a user-data
  write-protected DTT during the mapping check: the access-error handler
  returns a clean refusal and restores VBR/DFC/DTT before normal execution
  resumes. The production binary never installs this test mapping.
- In the same MMU-enabled guest, the production binary passes loaded-driver,
  released-reset, mismatched-ID, invalid-edit, RAM-backup and existing-backup
  guards. Dry run and verified backup leave the image unchanged. Forward
  programming changes exactly byte 0x10 from 0x48 to 0x49; reverse programming
  restores all 384 KiB exactly. Both report restored CPU/SCSI reset state.
  The persisted 512 KiB emulator image matches the restored mapped backup
  and retains the original unmapped 128 KiB. The emulator fixture uses
  HDH1234/IDH1234 because WinUAE supplies its own board serial.
- Physical hardware and whether the ID controls reset remain untested.
