.DEFAULT_GOAL := disk
include ../mk/build-config.mk

AGUIDECHECK=Tools/AGuideCheck/AGuideCheck
THIRDPARTY=../3rdparty
DEVTEST=$(THIRDPARTY)/devtest/devtest
TINYSETPATCH_DIR=$(THIRDPARTY)/TinySetPatch
TINYSETPATCH=$(TINYSETPATCH_DIR)/TinySetPatch
MMULIB_ARCHIVE=$(TINYSETPATCH_DIR)/downloads/MMULib.lha
MMULIB_FILES=$(addprefix $(TINYSETPATCH_DIR)/MMULib/Libs/,$(addsuffix .library,mmu 68020 68030 68040 68060))
RDBFLAGS_DIR=$(THIRDPARTY)/RDBFlags
RDBFLAGS=$(RDBFLAGS_DIR)/RDBFlags.gcc
AMIGA_CC ?= m68k-amigaos-gcc
AMIGA_STRIP ?= m68k-amigaos-strip
FULL_VERSION ?= $(shell git -C .. describe --tags --dirty | sed -r 's/^(release_|v)//')
YEAR ?= $(shell date +%Y)

WELCOME_CFLAGS := -Wall -Wextra -Os -fomit-frame-pointer -m68020-60 -msoft-float
WELCOME_CFLAGS += -DA4091_DISK_FULL_VERSION=\"$(FULL_VERSION)\"
WELCOME_CFLAGS += -DA4091_DISK_COPYRIGHT_YEAR=\"$(YEAR)\"
WELCOME_LDFLAGS := -noixemul -lamiga

# Enter through the parent so a standalone disk build gets its board tools.
board_a4091 := A4091
board_a4092 := A4092
board_a4770 := A4770
board_scsi710 := A4000T
board_scsi770 := A4000T770
DEVICE ?= $(if $(DEVNAME),$(board_$(DEVNAME)),A4091)
disk:
	$(MAKE) -C .. DEVICE="$(DEVICE)" disk

image: welcome scsifix $(DEVTEST) rdb $(RDBFLAGS) $(TINYSETPATCH) $(MMULIB_FILES)
	DEVICE=$(DEVICE) DEVNAME=$(DEVNAME) FULL_VERSION="$(FULL_VERSION)" ADATE="$(ADATE)" ./createdisk.sh

$(DEVTEST): FORCE
	$(MAKE) -s -C $(THIRDPARTY)/devtest OBJDIR=objs devtest
	m68k-amigaos-strip -s $@

rdb: $(THIRDPARTY)/bffs/rdb/rdb.c rdb_compat.h Makefile
	$(AMIGA_CC) -Os -m68000 -fno-strict-aliasing -fwrapv -noixemul \
		-include rdb_compat.h $< -lamiga -o $@
	$(AMIGA_STRIP) -s $@

$(RDBFLAGS): FORCE
	$(MAKE) -s -C $(RDBFLAGS_DIR) RDBFlags.gcc

clean:
	rm -f $(RDBFLAGS)
	rm -f *_??.??.adf
	rm -f NDK3.2.lha
	rm -rf Tools
	rm -f .welcome-config welcome scsifix.o scsifix rdb
	$(MAKE) -C $(TINYSETPATCH_DIR) clean
	$(MAKE) -C $(THIRDPARTY)/devtest clean

.PHONY: clean disk image check

check: $(AGUIDECHECK)
	vamos $(AGUIDECHECK) A4091.guide

$(AGUIDECHECK):
	wget http://aminet.net/dev/misc/NDK3.2.lha
	lha xqf NDK3.2.lha Tools/AGuideCheck/* Tools/AG2HTML/*

daynascsi.lha:
	wget https://github.com/RobSmithDev/daynaport-amiga/releases/download/1.3/daynascsi.lha

.welcome-config: CONFIG_TEXT := $(AMIGA_CC) $(WELCOME_CFLAGS) $(WELCOME_LDFLAGS) $(AMIGA_STRIP)
.welcome-config: FORCE
	$(write-config)

welcome: welcome.c Makefile .welcome-config
	$(AMIGA_CC) $(WELCOME_CFLAGS) $< $(WELCOME_LDFLAGS) -o $@
	$(AMIGA_STRIP) -s $@

# TinySetPatch derives its version from its own Git tags. Parent
# command-line variables also enter the environment, so clear both paths
# through which the driver's FULL_VERSION could reach the sub-build.
$(TINYSETPATCH): MAKEOVERRIDES =
$(TINYSETPATCH): FORCE
	env -u FULL_VERSION -u PROG_VERSION -u PROG_REVISION \
		$(MAKE) -s -C $(TINYSETPATCH_DIR) TinySetPatch

$(MMULIB_ARCHIVE):
	$(MAKE) -s -C $(TINYSETPATCH_DIR) downloads/MMULib.lha

$(MMULIB_FILES): $(MMULIB_ARCHIVE) | $(TINYSETPATCH_DIR)/MMULib/Libs
	cd $(TINYSETPATCH_DIR) && lha xqf downloads/MMULib.lha MMULib/Libs/$(@F)
	touch -c $@

$(TINYSETPATCH_DIR)/MMULib/Libs:
	mkdir -p $@

scsifix.o: scsifix.asm
	vasmm68k_mot -Fhunk -o scsifix.o scsifix.asm
scsifix: scsifix.o
	vlink -bamigahunk -o scsifix -s scsifix.o
