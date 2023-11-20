# A4091 Firmware & Factory Software

This package contains source code to build an Open Source version of the AmigaOS
driver, ROM image, test utility, and driver debug utility for the A4091 Zorro III
Advanced SCSI disk controller.

## A4091 AutoConfig(tm) ROM

On the A4091 the ROM contains both the autoconfig data needed to probe the
device, and the device driver (`a4091.device` or `2nd.scsi.device`). On the
original A4091, the ROM is a 32KB 8 bit ROM. On A4091 REV B boards it is
possible to use either a 32K or 64K ROM using `J100` to switch.

On the ReA4091 it is recommended to use Winbond W27C512 EEPROMs because
UV erasing EPROMs is somewhat cumbersome. Also, the additional 32KB
ROM can be used to store a CDFileSystem to boot from CD-ROM.

## How to build

### Prerequisites

You should install the latest version of [Bebbo's amiga-gcc](https://github.com/bebbo/amiga-gcc) to compile this code. We have previously used this setup on a Linux (Ubuntu) machine as well as a MacBook. On the latter you will need XCode and [HomeBrew](https://brew.sh) installed.

After checking out the source code, you will have to update all the git
submodules used by this package. To do so, please run

```
$ cd a4091-software
$ git submodule update --init
```

### Compile Time Configuration

There are a few knobs to configure the ROM image you are building.

Most prominently, you can enable debugging output through your Amiga's serial
port. On a production image this is not required and should be turned off:

```
#CFLAGS  += -DDEBUG             # Show basic debug
#CFLAGS  += -DDEBUG_SYNC        # Show Synchronous SCSI debug
#CFLAGS  += -DDEBUG_CMD         # Show handler commands received
#CFLAGS  += -DDEBUG_CALLOUT     # Show callout (timeout abort) services
# Per file debugging
#CFLAGS  += -DDEBUG_ATTACH      # Debug attach.c
#CFLAGS  += -DDEBUG_DEVICE      # Debug device.c
#CFLAGS  += -DDEBUG_CMDHANDLER  # Debug cmdhandler.c
#CFLAGS  += -DDEBUG_NCR53CXXX   # Debug ncr53cxxx.c
#CFLAGS  += -DDEBUG_PORT        # Debug port.c
#CFLAGS  += -DDEBUG_SCSIPI_BASE # Debug scsipi_base.c
#CFLAGS  += -DDEBUG_SCSICONF    # Debug scsiconf.c
#CFLAGS  += -DDEBUG_SCSIMSG     # Debug scsimsg.c
#CFLAGS  += -DDEBUG_SD          # Debug sd.c
#CFLAGS  += -DDEBUG_SIOP        # Debug siop.c
#CFLAGS  += -DDEBUG_MOUNTER     # Debug mounter.c
#CFLAGS  += -DDEBUG_BOOTMENU    # Debug bootmenu.c
#CFLAGS  += -DNO_SERIAL_OUTPUT  # Turn off serial debugging for the whole driver
```

### Compiling

Type `make` to compile the driver or `make verbose` to also see all the
compiler invocations.

```
mkdir -p objs
Building objs/device.o
Building objs/ncr53cxxx
Generating objs/siop_script.out
Building objs/siop.o
Building objs/port.o
Building objs/attach.o
Building objs/cmdhandler.o
Building objs/printf.o
Building objs/sd.o
Building objs/scsipi_base.o
Building objs/scsiconf.o
Building objs/scsimsg.o
Building objs/mounter.o
Building objs/bootmenu.o
Building objs/romfile.o
Building objs/battmem.o
Building objs/reloc.o
Building objs/version.o
Building a4091.device
a4091.device is 46384 bytes
Building objs/rnc
Compressing a4091.device ... 46384 -> 29321 bytes
Building objs/a4091.o
Building a4091
Building objs/a4091d.o
Building a4091d
Building objs/rom.o
Building a4091_nodriver.rom
ROM a4091_nodriver.rom fits in 64k
Building objs/romtool
Building a4091.rom
a4091.rom: 64kB A4091 ROM image. Signature: OK

 ROM header:   offset = 0x000000 length = 0x0006b0
 a4091.device: offset = 0x0006b0 length = 0x007290 compressed (b530 uncompressed)
 CDFileSystem: offset = 0x000000 length = 0x000000

 34472 bytes free (52.60%)

Compressing BootCDFileSystem ... 19248 -> 12274 bytes
Building a4091_cdfs.rom
a4091_cdfs.rom: 64kB A4091 ROM image. Signature: OK

 ROM header:   offset = 0x000000 length = 0x0006b0
 a4091.device: offset = 0x0006b0 length = 0x007290 compressed (b530 uncompressed)
 CDFileSystem: offset = 0x007940 length = 0x003000 compressed (4b30 uncompressed)

 22184 bytes free (33.85%)

$
```

This will generate the following files:


| File               |   Description                              |
|--------------------|--------------------------------------------|
| a4091.device       | device driver (i.e. for loading from disk) |
| a4091.rom          | ROM driver to be written to a W27C512      |
| a4091_nodriver.rom | ROM image with no driver (for dev/test)    |
| a4091              | Command line utility to probe the board    |
| a4091d             | Debugging daemon to attach to the driver   |


In addition to the ROM image a4091.rom, you will notice `a4091_nodriver.rom`
was built above. This image may be useful if you want to load the driver
off a floppy disk but still want a ROM image so that the A4091 card shows
up during AutoConfig(tm). It's also useful if you want to use `a4091 -t`
to run diagnostics on a card, since that utility will operate in conflict
with a driver.

For test purposes it is possible to build a ROM using the original
`2nd.scsi.device` driver. To do that, you will need the extracted driver
in your build directory named `a3090.ld_strip`. The Makefile will detect
this file and automatically build a `a4091_commodore.rom` in addition to
the standard open source ROM.


## CDROM Boot Support

This A4091 driver supports booting from CDROM. In order to achieve that,
you will need a supported CDFileSystem in your ROM.

The smallest working CDFileSystem is `BootCDFileSystem` from your Amiga
Forever CD or the AmigaOS 4.x Boot Floppy. Place that file in your source
directory, and the Makefile will then also build a `a4091_cdfs.rom` ROM
image.

| Origin           | Version                        | Works
|------------------+--------------------------------+----------
| BootCDFileSystem | CDFileSystem 50.21 (30.8.2003) | YES
| AmigaOS 3.2.2    | CDFileSystem 47.28             | YES

You can add another CDFileSystem to a ROM using romtool:

```
$ objs/rnc p CDFileSystem CDFileSystem.rnc -m 1
-= RNC ProPackED v1.8 [by Lab 313] (01/26/2021) =-
-----------------------------
File successfully packed!
Original/new size: 33016/21396 bytes
$ objs/romtool a4091_cdfs.rom -F CDFileSystem.rnc
a4091_cdfs.rom: 64kB A4091 ROM image. Signature: OK

 ROM header:   offset = 0x000000 length = 0x0006b0
 a4091.device: offset = 0x0006b0 length = 0x007290 compressed (b530 uncompressed)
 CDFileSystem: offset = 0x007940 length = 0x0053a0 compressed (80f8 uncompressed)

 13064 bytes free (19.93%)

$
```


## Flashing / Programming the ROM

If your ROM file fits in 32k you can use a 27C256 EPROM. If the ROM is 64k,
use a W27C512 EEPROM (or EPROM).

We have used a [Galep 5](http://www.conitec.net/english/galep5.php)
but a Super MiniPRO TL866II Plus or any other EPROM programmer will
probably work as well.

## Creating the floppy disk

You can create a floppy disk for your A4091. Unlike the original it will not
contain an updated version of `68040.library` and `SetPatch`, but some
documentation and debug tools useful for your A4091. To create an ADF file that
you can write to a floppy, please use

```
$ cd disk
$ make disk
./createdisk.sh
Looking for submodules...
Building devtest...
Building objs/devtest.o
Building devtest
Extracting rdb...
Creating disk...
Cleaning up...
Done. Please verify disk contents of A4091_4227.adf below:
------------------------------------------------------------------------------------------
Amiga4091                                        VOLUME  --------  05.11.2023 15:38:16.00  DOS0:ofs #512
  A4091.guide                                     15328  ----rwed  05.11.2023 15:38:16.00
  A4091.guide.info                                  523  ----rwed  05.11.2023 15:38:16.00
  Disk.info                                         364  ----rwed  05.11.2023 15:38:16.00
  Devs                                              DIR  ----rwed  05.11.2023 15:38:16.00
    a4091.device                                  46384  ----rwed  05.11.2023 15:38:16.00
  S                                                 DIR  ----rwed  05.11.2023 15:38:16.00
    Startup-Sequence                               1009  ----rwed  05.11.2023 15:38:16.00
  Tools                                             DIR  ----rwed  05.11.2023 15:38:15.00
    a4091                                         66524  ----rwed  05.11.2023 15:38:15.00
    a4091d                                        47824  ----rwed  05.11.2023 15:38:15.00
    devtest                                       65412  ----rwed  05.11.2023 15:38:15.00
    rdb                                           26816  ----rwed  05.11.2023 15:38:15.00
    RDBFlags                                       2208  ----rwed  05.11.2023 15:38:15.00
sum:           582  291Ki        297984
data:          564  282Ki        288768  96.91%
fs:             18  9.0Ki          9216   3.09%
------------------------------------------------------------------------------------------
Created A4091_4227.adf
$
```

## Internals

### ROM access

The A4091 uses an 8bit ROM and with AutoConfig implementations on the A3000 and
A4000 it can only be accessed nibble wide. Hence, the A4091 ROM will relocate
the driver to RAM because it is not possible to execute in place. This has a
positive speed impact but makes the ROM startup code a little bit more
cumbersome.

### a4091.device

The device driver is based off the NetBSD NCR53c710 driver and has been adapted
to AmigaOS.

### Boot menu

The ROM contains a diagnostic menu that you can reach by holding down the right
mouse button during boot. The menu can show you dip switch settings and also
attached SCSI devices. There is also an option in the menu to enable or
disable CD-ROM boot.

### a4091 tool

The `a4091` tool can be used to probe the board and detect possible hardware
issues.

`a4091 -t` runs all tests once. If the A4091 is functioning properly, every test will pass.

`a4091 -t -L` will run all tests in a continuous loop while counting passes. If you built a board yourself, doing at least 500 passes is recommended. You can skip to individual test(s) by appending one or more numbers between `0` and `8` to `-t`, e.g., `a4091 -t56` will only run tests number 5 and 6. **Note:** If you skip **failing** tests, consecutive tests may produce unexpected results.

### Source files

Files will be documented here in an order to help understand code flow.

`rom.S` contains initialization code called during OS startup. It loads the rest of the driver from the A4091 ROM. `reloc.S` is called to do runtime relocation of the loaded driver's code and data.

`device.c` implements startup and has standard trackdisk entry points. If started from ROM, it calls out to drive mounting and the boot menu. During normal operation, it calls into cmdhandler.c for I/O operations.

`mounter.c` probes for drives, finds partitions, and mounts them.

`bootmenu.c` provides the optional diagnostic menu at boot time.

`cmdhandler.c` implements the task which fields all incoming I/O requests, calling into attach.c for mounting drives, and sd.c for various SCSI I/O operations.

`attach.c` probes a specified SCSI target and creates data structures needed by the NetBSD SCSI code for managing that device.

`sd.c` creates SCSI requests (xs data structure) and calls into the NetBSD scsipi_base.c for queuing and processing. It also implements callbacks for I/O complete. The callbacks, such as `sd_complete()` call back into cmd_complete() in cmdhandler.c. That function replies to the AmigaOS task which made the initial request.

`scsi`* files are NetBSD's core SCSI stack. We've tried to keep all modifications to this code within `#ifdef PORT_AMIGA` in order to make it easier to apply updates.

`siop.c` is the NetBSD driver for the 53C710 SCSI controller on the A4091. We've also tried to keep all modifications to this code within `#ifdef PORT_AMIGA` in order to make it easier to apply updates. Probably not as well as the `scsi`* files above. The driver uses an internal structure, the acb, to maintain the queue of SCSI requests (both queued and issued) to the 53C710. The callback into the higher level stack is `scsipi_done()` for operations which have completed or failed.

`printf.c` contains code which, when the driver is compiled for debug, can emit output on the Amiga serial port (9600 8 N 1). Driver code which calls printf() will use this code. When no Makefile DEBUG flag is specified, calls to printf() do not add any size to the executable.

`version.h` is manually updated to change the compiled-in driver version number.

`port.c` contains miscellaneous functions to support the port from NetBSD to AmigaOS.

`siop_script.ss` contains the SCRIPTS processor source code. It is taken unmodified from the NetBSD driver, and is compiled by `ncr53cxxx` into C source which is then built as part of the driver.

`ncr53cxxx.c` is the source to the NetBSD SCRIPTS compiler, with minor fixes.

`rom.ld` is the linker directive file which tells how to assemble the ROM image.

`rnc.S` is a small RNC ProPack decompressor that is used to maximize rom space.

`romfile.c` handles "files" in the ROM and is used to find the CDFileSystem at
boot.

`romtool.c` is a utility to manipulate A4091 rom images. It lets you remove/add
device drivers and filesystems.

