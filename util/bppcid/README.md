# bppcid

An experimental Blizzard PPC 68060 ROM identity editor for testing an added
NCR710 controller. It changes only the first character of the seven-character
ID at address `00f00010`: H (without SCSI) to I (with SCSI), or the reverse.
It neither adds a driver nor changes programmable logic. Whether the ID
controls SCSI enablement on a physical board remains unproven.

Build with `make -C util/bppcid`; the top-level `make` builds it too.
Run host fault-injection tests with `make -C util/bppcid test`.

For a minimal automatic backup floppy, run `make -C util/bppcid disk`.
This creates `util/bppcid/blizzard-backup.adf` with only `bppcid`, `ncr7xx`,
a startup-sequence and instructions. It has room to save the 384 KiB backup
and logs on the same disk. There are no A4091 support files. An existing
output ADF is never overwritten; use `ADF=/path/to/new-image.adf` to choose
another filename. `XDFTOOL` can select the host `xdftool` executable.

Boot the write-enabled backup floppy with **S held**, allowing its own
startup-sequence to run. It saves and verifies `SYS:bppc-backup.bin`, then
runs `ncr7xx -a bppc -d -R`; output goes to `backup.log` and `probe.log`.
An existing backup prevents both commands from running again, preserving
the files. See `backup-disk/ReadMe` for failure and retry instructions.

## On the Amiga

Boot without startup-sequence, holding S to keep SCSI disabled. The utility
refuses a loaded blizzppc.device or PPC runtime, MMU remapping/write protection
of ROM or board I/O, or released SCSI reset. It checks every page with the
68060 PLPAR/PLPAW instructions before issuing flash commands. It supports only
AMD JEDEC ID 01:a4 (Am29F040/Am29F040B).

Inspect the board and preview the proposed change first:

```
bppcid
bppcid --set-id IAH0029 --expect-id HAH0029
```

Both commands are read-only: no flash command or erase is issued. To perform
the proposed edit, specify a NEW backup filename on persistent storage:

```
bppcid --set-id IAH0029 --expect-id HAH0029 --backup DH0:bppc-original.bin --write
```

Replace DH0 with an accessible disk with at least 384 KiB free. The utilities
floppy generally does not have enough spare space for this backup. RAM: and
assigns pointing to RAM: are rejected for write operations. Keep an additional
copy of the backup off the Amiga. Other RAM-backed devices such as RAD: are
also unsuitable, even if the handler is not recognized as RAM:.

The backup is a raw 384 KiB image of `00f00000..00f5ffff`. The remaining
128 KiB of the 512 KiB flash is not exposed by this mapping: addresses
`00f60000..00f7ffff` contain board I/O and must not be dumped as ROM.
The backup contains the entire sector being rewritten, all mapped firmware
and the configuration sector. It is not a complete 512 KiB programmer image.

The program creates the backup exclusively (existing files are never
overwritten), closes it, reopens it and verifies every byte before writing.
It reports a CRC32 so the saved file can also be checked on another machine.
To create just a backup, use `bppcid --backup DH0:another-new-file.bin`.

Changing 0x48 to 0x49 requires erasing the first 64 KiB boot sector. The
program temporarily holds the PPC in reset, disables caches and interrupts,
uses RAM-resident code to rewrite that sector, then checks all 384 KiB against
the backup, allowing exactly the requested identity byte to differ. The
firmware checksum excludes that byte, so it is preserved unchanged.

Do not interrupt power or reset during the write. A programming/verification
failure triggers one attempt to restore the original sector. The output
distinguishes a verified restoration from failed recovery. A power loss or
failed recovery may require an external programmer; the backup cannot make
an interrupted boot-sector erase atomic.

After a successful verified write, fully power off and boot again. Confirm
the ROM ID and test the board with:

```
ncr7xx -a bppc -d -r
ncr7xx -a bppc -R
```

To reverse the experiment, boot with S held again and use a different backup:

```
bppcid --set-id HAH0029 --expect-id IAH0029 --backup DH0:bppc-with-I.bin --write
```

## Evidence and validation

The minimal backup disk was boot-tested in PPC-capable WinUAE with a
68060 MMU and Kickstart 3.2.3. Its saved 393216-byte backup matched the
emulator's mapped ROM byte for byte, and the reset probe produced a log.
A second boot left the entire test ADF unchanged, preserving the backup
and both logs. This validates the disk workflow in emulation; physical
reset-release behavior remains a separate hardware question.

Host flash fault-injection tests and WinUAE tests with 68060 MMU emulation
pass, including write-protection refusal, a one-byte forward edit, and an
exact reverse restoration. Physical hardware remains untested.

See DESIGN.md for updater addresses, memory-map evidence and test details. The reference is the supplied BlizzPPC060Update executable; it is not
invoked or modified by this utility. WinUAE overwrites board identification
on startup, so emulator flash validation cannot establish whether this edit
fixes physical SCSI enablement.
