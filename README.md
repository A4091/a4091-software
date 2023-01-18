# A4091 Firmware & Factory Software

Source code to build an Open Source version of the AmigaOS driver, ROM image,
test utility, and driver debug utility for the A4091 Zorro III Advanced SCSI
disk controller.

## A4091 AutoConfig(tm) ROM

On the A4091 the ROM contains both the autoconfig data needed to probe the
device, and the device driver (`a4091.device` or `2nd.scsi.device`). On the
original A4091, the ROM is a 32KB 8 bit ROM. On A4091 REV B boards it is
possible to use either a 32K or 64K ROM using `J100` to switch.

On the ReA4091 we have mostly used Winbond W27C512 EEPROM parts because dealing
with the erasing of EPROMs is somewhat cumbersome. Also, the additional 32KB
ROM can be used to store a CDFileSystem to boot from CD-ROM.

## How to build

### Prerequisites

You should install the latest version of [Bebbo's amiga-gcc](https://github.com/bebbo/amiga-gcc) to compile this code. We have previously used this setup on a Linux (Ubuntu) machine as well as a MacBook. On the latter you will need XCode and [HomeBrew](https://brew.sh) installed.

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
a4091.device is 34468 bytes
Building objs/a4091.o
Building a4091
Building objs/a4091d.o
Building a4091d
Building objs/rom.o
Building objs/assets.o
Building a4091.rom
ROM a4091.rom fits in 64k
Building objs/rom_nd.o
Building objs/assets_nd.o
Building a4091_nodriver.rom
ROM a4091_nodriver.rom fits in 32k
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

Last but not least, this A4091 driver supports booting from CDROM (beta alert).
In order to achieve that, you will need `BootCDFileSystem` from your Amiga
Forever CD or the AmigaOS 4.x Boot Floppy. Place that file in your source
directory, and the Makefile will then also build a `a4091_cdfs.rom` ROM image.

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
$ ./createdisk.sh
Downloading...
Building...
Building objs/devtest.o
Building devtest
Creating disk...
Cleaning up...
Done:
Amiga4091                                        VOLUME  --------  12.11.2022 11:20:45.00  DOS0:ofs #512
  A4091.guide                                     13916  ----rwed  12.11.2022 11:20:45.00
  A4091.guide.info                                  523  ----rwed  12.11.2022 11:20:45.00
  Disk.info                                         364  ----rwed  12.11.2022 11:20:45.00
  Tools                                             DIR  ----rwed  12.11.2022 11:20:45.00
    a4091                                         53356  ----rwed  12.11.2022 11:20:44.00
    a4091d                                        47672  ----rwed  12.11.2022 11:20:45.00
    devtest                                       65212  ----rwed  12.11.2022 11:20:45.00
    rdb                                           26816  ----rwed  12.11.2022 11:20:44.00
sum:           441  220Ki        225792
data:          429  214Ki        219648  97.28%
fs:             12  6.0Ki          6144   2.72%
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

### Boot Menu

The ROM contains a diagnostic menu that you can reach by holding down the right
mouse button during boot. The menu can show you dip switch settings and also
attached SCSI devices. There is also an option in the menu to enable or
disable CD-ROM boot.

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
