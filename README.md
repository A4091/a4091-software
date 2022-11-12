# A4091 Firmware & Factory Software

Source code to build an Open Source version of the AmigaOS driver, ROM image,
test utility, and driver debug utility for the A4091 Zorro III Advanced SCSI
disk controller.

## A4091 AutoConfig(tm) ROM

On the A4091 the ROM contains both the autoconfig data needed to probe the
device, and the device driver (`a4091.device` or `2nd.scsi.device`). On the
original A4091, the ROM is a 32KB 8 bit ROM. On A4091 REV B boards it is
possible to use either a 32K or 64K ROM using `J100` to switch.

On the ReA4091 we have mostly used Winbond 27C512 EEPROM parts because dealing
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
#CFLAGS  += -DDEBUG_CMDHANDLER  # Debug commandhandler.c
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

For test purposes it is possible to use the original `2nd.scsi.device` driver
instead of the Open Source version. To do that, you will need the extracted
driver in your build directory named `a3090.ld_strip` and `COMMODORE_DEVICE`
enabled in the build flags.

It is also possible to compile the ROM image without a driver at all. This is
useful if you want to load the driver off a floppy disk but still want a ROM
image so that the A4091 card shows up during AutoConfig(tm). In that case,
enable `NO_DEVICE`:

```
# Enable to put the original Commodore driver into the ROM
# (You will have to extract it yourself)
#ROMDRIVER := -DCOMMODORE_DEVICE=1
#ROMDRIVER := -DNO_DEVICE=1
```

Last but not least, this A4091 driver supports booting from CDROM (beta alert).
In order to achieve that, you will need `BootCDFileSystem` from your Amiga
Forever CD or the AmigaOS 4.x Boot Floppy. With that file in your source
directory, enable `CDFS` in the Makefile and rebuild the ROM:

```
#CDFILESYSTEM := -DCDFS=1
```

### Compiling

Type `make` to compile the driver or `make verbose` to also see all the
compiler calls.

```
$ make
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
Building objs/a4091.o
Building a4091
Building objs/a4091d.o
Building a4091d
Building objs/rom.o
Building objs/assets.o
Building a4091.rom
a4091.device is 34400 bytes
ROM fits in 64k
$
```

This will generate the following files:


| File         |   Description                              |
|--------------|--------------------------------------------|
| a4091.device | device driver (i.e. for loading from disk) |
| a4091.rom    | ROM driver to be written to a W27C512      |
| a4091        | Command line utility to probe the board    |
| a4091d       | Debugging daemon to attach to the driver   |

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
mouse button during boot.

