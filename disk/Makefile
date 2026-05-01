AGUIDECHECK=Tools/AGuideCheck/AGuideCheck
THIRDPARTY=../3rdparty
DEVTEST=$(THIRDPARTY)/devtest/devtest
TINYSETPATCH_DIR=$(THIRDPARTY)/TinySetPatch
TINYSETPATCH=$(TINYSETPATCH_DIR)/TinySetPatch
MMULIB_MAIN=$(TINYSETPATCH_DIR)/MMULib/Libs/mmu.library
AMIGA_CC ?= m68k-amigaos-gcc
AMIGA_STRIP ?= m68k-amigaos-strip
FULL_VERSION ?= $(shell git -C .. describe --tags --dirty | sed -r 's/^(release_|v)//')
YEAR ?= $(shell date +%Y)

WELCOME_CFLAGS := -Wall -Wextra -Os -fomit-frame-pointer -m68020-60 -msoft-float
WELCOME_CFLAGS += -DA4091_DISK_FULL_VERSION=\"$(FULL_VERSION)\"
WELCOME_CFLAGS += -DA4091_DISK_COPYRIGHT_YEAR=\"$(YEAR)\"
WELCOME_LDFLAGS := -noixemul -lamiga

disk: welcome scsifix $(DEVTEST) rdb $(TINYSETPATCH) $(MMULIB_MAIN)
	DEVICE=$(DEVICE) DEVNAME=$(DEVNAME) FULL_VERSION="$(FULL_VERSION)" ADATE="$(ADATE)" ./createdisk.sh

$(DEVTEST): FORCE
	$(MAKE) -s -C $(THIRDPARTY)/devtest
	m68k-amigaos-strip -s $@

rdb: $(THIRDPARTY)/bffs/dist/bffs16_src.lha
	lha xiq2f $< bffs_1.6/bin/rdb

clean:
	rm -f *_??.??.adf
	rm -f NDK3.2.lha
	rm -rf Tools
	rm -f welcome scsifix.o scsifix rdb
	$(MAKE) -C $(TINYSETPATCH_DIR) clean
	$(MAKE) -C $(THIRDPARTY)/devtest clean

FORCE:
.PHONY: clean disk FORCE

check: $(AGUIDECHECK)
	vamos $(AGUIDECHECK) A4091.guide

$(AGUIDECHECK):
	wget http://aminet.net/dev/misc/NDK3.2.lha
	lha xqf NDK3.2.lha Tools/AGuideCheck/* Tools/AG2HTML/*

daynascsi.lha:
	wget https://github.com/RobSmithDev/daynaport-amiga/releases/download/1.3/daynascsi.lha

welcome: welcome.c
	$(AMIGA_CC) $(WELCOME_CFLAGS) $< $(WELCOME_LDFLAGS) -o $@
	$(AMIGA_STRIP) -s $@

$(TINYSETPATCH):
	$(MAKE) -s -C $(TINYSETPATCH_DIR) TinySetPatch

$(MMULIB_MAIN):
	$(MAKE) -s -C $(TINYSETPATCH_DIR) download-libs

scsifix.o: scsifix.asm
	vasmm68k_mot -Fhunk -o scsifix.o scsifix.asm
scsifix: scsifix.o
	vlink -bamigahunk -o scsifix -s scsifix.o
