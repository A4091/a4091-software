NOW     := $(shell date '+%Y-%m-%d %H:%M:%S')
DATE    := $(firstword $(NOW))
TIME    := $(lastword $(NOW))
ADATE   := $(shell date '+%-d.%-m.%Y')
# FULL_VERSION is 42.xx-yy-dirty
FULL_VERSION ?= $(shell git describe --tags --dirty | sed -r 's/^release_//')
DEVICE_VERSION := $(shell echo $(FULL_VERSION) | cut -f1 -d\.)
DEVICE_REVISION := $(shell echo $(FULL_VERSION) | cut -f2 -d\.|cut -f1 -d\-)

# Default to NCR53C770 driver:

#DEVICE := A4000T
#DEVICE := A4000T770
#DEVICE := A4092
DEVICE ?= A4091

ifeq ($(DEVICE),A4091)
TARGET  := NCR53C710
TARGETCFLAGS := -DDRIVER_A4091 -DNCR53C710=1 -DDEVNAME="a4091" -DNO_CONFIGDEV=0 -DHAVE_ROM=1
TARGETAFLAGS := -DNCR53C710=1 -DDRIVER_A4091=1
DEVNAME=a4091
HAVE_ROM=y
else ifeq ($(DEVICE),A4092)
TARGET  := NCR53C710
TARGETCFLAGS := -DDRIVER_A4092 -DNCR53C710=1 -DDEVNAME="a4092" -DNO_CONFIGDEV=0 -DHAVE_ROM=1 -DFLASH_SPI=1 # -DFLASH_PARALLEL=1
TARGETAFLAGS := -DNCR53C710=1 -DDRIVER_A4092=1
DEVNAME=a4092
HAVE_ROM=y
else ifeq ($(DEVICE),A4000T)
TARGET  := NCR53C710
TARGETCFLAGS := -DDRIVER_A4000T -DNCR53C710=1 -DDEVNAME="scsi710" -DNO_CONFIGDEV=1 -DHAVE_ROM=0
TARGETAFLAGS := -DNCR53C710=1
DEVNAME=scsi710
HAVE_ROM=n
else ifeq ($(DEVICE),A4000T770)
TARGET  := NCR53C770
TARGETCFLAGS := -DDRIVER_A4000T770 -DNCR53C770=1 -DDEVNAME="scsi770" -DNO_CONFIGDEV=1 -DHAVE_ROM=0
TARGETAFLAGS := -DNCR53C770=1
DEVNAME=scsi770
HAVE_ROM=n
else
$(error Unknown build target! Please set DEVICE to A4091, A4092, A4000T or A4000T770.)
endif

OBJDIR  := objs

ROM	:= $(DEVNAME).rom
ROM_ND	:= $(DEVNAME)_nodriver.rom
ROM_DB	:= $(DEVNAME)_debug.rom
ROM_CD	:= $(DEVNAME)_cdfs.rom
KICK    := $(DEVNAME).kick
PROG	:= $(DEVNAME).device
PROGU	:= ncr7xx
PROGD	:= a4091d
SRCS    := device.c version.c port.c attach.c cmdhandler.c printf.c
SRCS    += sd.c scsipi_base.c scsipiconf.c scsiconf.c scsimsg.c 3rdparty/mounter/mounter.c bootmenu.c
ifeq ($(TARGET),NCR53C710)
SRCS    += siop.c
else ifeq ($(TARGET),NCR53C770)
SRCS    += siop2.c
endif
ifeq ($(DEVNAME),a4092)
SRCS    += util/a4092flash/flash.c util/a4092flash/nvram_flash.c util/a4092flash/spi.c
endif
SRCS    += romfile.c battmem.c
ASMSRCS := reloc.S
SRCSU   := ncr7xx.c
SRCSD   := a4091d.c
OBJS    := $(SRCS:%.c=$(OBJDIR)/%.o)
OBJSD   := $(SRCSD:%.c=$(OBJDIR)/%.o)
OBJSU   := $(SRCSU:%.c=$(OBJDIR)/%.o)
ASMOBJS := $(ASMSRCS:%.S=$(OBJDIR)/%.o)
OBJSROM := $(OBJDIR)/rom.o
TOOLS   := $(PROGU) $(PROGD) util/a4092flash/a4092flash

HOSTCC  ?= cc
CC      := m68k-amigaos-gcc
STRIP   := m68k-amigaos-strip
VLINK   := vlink
VASM    := vasmm68k_mot

# Find an appropriate set of NDK includes
NDK_PATH ?= $(shell realpath $$(dirname $$(which $(CC)))/../m68k-amigaos/ndk-include)

# CFLAGS for device driver
#
CFLAGS  := -DBUILD_DATE=\"$(DATE)\" -DBUILD_TIME=\"$(TIME)\" -DAMIGA_DATE=\"$(ADATE)\"
CFLAGS  += -D_KERNEL -DPORT_AMIGA -DA4091 $(TARGETCFLAGS) -I. -I3rdparty/mounter
#DEBUG  += -DDEBUG             # Show basic debug
#DEBUG  += -DDEBUG_SYNC        # Show Synchronous SCSI debug
#DEBUG  += -DDEBUG_CMD         # Show handler commands received
#DEBUG  += -DDEBUG_CALLOUT     # Show callout (timeout abort) services
# Per file debugging
#DEBUG  += -DDEBUG_ATTACH      # Debug attach.c
#DEBUG  += -DDEBUG_DEVICE      # Debug device.c
#DEBUG  += -DDEBUG_CMDHANDLER  # Debug cmdhandler.c
#DEBUG  += -DDEBUG_PORT        # Debug port.c
#DEBUG  += -DDEBUG_SCSIPI      # Debug with SCSIPI_DEBUG_FLAGS
#DEBUG  += -DDEBUG_SCSIPI_BASE # Debug scsipi_base.c / scsipiconf.c
#DEBUG  += -DDEBUG_SCSICONF    # Debug scsiconf.c
#DEBUG  += -DDEBUG_SCSIMSG     # Debug scsimsg.c
#DEBUG  += -DDEBUG_SD          # Debug sd.c
#DEBUG  += -DDEBUG_SIOP        # Debug siop.c
#DEBUG  += -DDEBUG_MOUNTER     # Debug mounter.c
#DEBUG  += -DDEBUG_BOOTMENU    # Debug bootmenu.c
#DEBUG  += -DDEBUG_FLASH       # Debug util/a4092flash/*
#DEBUG  += -DNO_SERIAL_OUTPUT  # Turn off serial debugging for the whole driver
CFLAGS  += $(DEBUG)
CFLAGS  += -DENABLE_SEEK  # Not needed for modern drives (~500 bytes)
#CFLAGS  += -DDISKLABELS  # Enable support for MBR / GPT disklabels
#CFLAGS  += -DENABLE_QUIRKS
CFLAGS  += -DENABLE_QUICKINTS # Disable for A4091 Mini
CFLAGS  += -Os -fomit-frame-pointer -noixemul
#CFLAGS  += -fbaserel -resident -DUSING_BASEREL
CFLAGS  += -msmall-code
CFLAGS  += -Wall -Wextra -Wno-pointer-sign
CFLAGS += -mcpu=68060
CFLAGS += -DDEVICE_VERSION=$(DEVICE_VERSION)
CFLAGS += -DDEVICE_REVISION=$(DEVICE_REVISION)
CFLAGS += -DDEVICE_VERSION=$(DEVICE_VERSION)
CFLAGS += -DFULL_VERSION="$(FULL_VERSION)"

CFLAGS_TOOLS := -Wall -Wextra -Wno-pointer-sign -fomit-frame-pointer -Os -mcpu=68060
CFLAGS_TOOLS += -DAMIGA_DATE=\"$(ADATE)\"

LDFLAGS_COMMON = -Wl,-Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst
LDFLAGS        = -nostartfiles -nostdlib -ldebug -lgcc -lc -lamiga -ramiga-dev $(LDFLAGS_COMMON)
LDFLAGS_TOOLS  = -mcrt=clib2 -lgcc -lc -lamiga $(LDFLAGS_COMMON)

#CFLAGS  += -g
#LDFLAGS += -g

#CFLAGS_TOOLS  += -g
#LDFLAGS_TOOLS += -g

# Add link-time optimization (must also have optimization specified in LDFLAGS)
# -flto

# If verbose is specified with no other targets, then build everything
ifeq ($(MAKECMDGOALS),verbose)
verbose: all
endif
ifeq (,$(filter verbose timed, $(MAKECMDGOALS)))
QUIET   := @
else
QUIET   :=
endif

SC_ASM	:= $(OBJDIR)/ncr53cxxx

ifeq ($(TARGET),NCR53C710)
SIOP_SCRIPT := $(OBJDIR)/siop_script.out
else ifeq ($(TARGET),NCR53C770)
SIOP_SCRIPT := $(OBJDIR)/siop2_script.out
endif

red=\033[1;31m
green=\033[1;32m
yellow=\033[1;33m
end=\033[0m

ifeq (, $(shell which $(CC) 2>/dev/null ))
$(error "No $(CC) in PATH: maybe do PATH=$$PATH:/opt/amiga/bin")
endif

# Handle git submodules
GIT:=$(shell git -C "$(CURDIR)" rev-parse --git-dir 1>/dev/null 2>&1 \
        && command -v git)
ifneq ($(GIT),)
freshsubs:=$(shell git submodule update --init $(quiet_errors))
endif

# Autodetect which CDFileSystem is available if any. This mechanism
# lets you build with an AmigaOS CDFileSystem if you drop the file
# into the source tree. Per default, we are building with CDVDFS.

ifneq (,$(wildcard BootCDFileSystem))
CDFS=BootCDFileSystem
else
ifneq (,$(wildcard CDFileSystem))
CDFS=CDFileSystem
else
CDFS=$(OBJDIR)/CDVDFS
endif
endif

ifeq ($(HAVE_ROM),y)
ROMS:=$(ROM) $(ROM_CD)
else
# Build relocatable kickstart module if we don't have to build a ROM image
ROMS:=$(KICK)
endif

all: $(PROG) $(ROMS) $(TOOLS)

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(OBJDIR)/%,$(filter-out $(OBJDIR)/%,$(basename $(1)).o)) $(filter $(OBJDIR)/%,$(basename $(1)).o): $(1)
endef
$(foreach SRCFILE,$(SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE))))
$(foreach SRCFILE,$(SRCSU),$(eval $(call DEPEND_SRC,$(SRCFILE))))
$(foreach SRCFILE,$(SRCSD),$(eval $(call DEPEND_SRC,$(SRCFILE))))

$(OBJDIR)/version.o: version.h $(filter-out $(OBJDIR)/version.o, $(OBJS) $(ASMOBJS))
$(OBJDIR)/siop.o: $(OBJDIR)/siop_script.out
$(OBJDIR)/siop2.o: $(OBJDIR)/siop2_script.out
$(OBJDIR)/siop.o:: CFLAGS += -I$(OBJDIR)
$(OBJDIR)/siop2.o:: CFLAGS += -I$(OBJDIR)
$(OBJDIR)/a4091d.o:: CFLAGS_TOOLS += -D_KERNEL -DPORT_AMIGA -Wno-format
$(OBJDIR)/ncr7xx.o:: CFLAGS_TOOLS += -Wno-format

# XXX: Need to generate real dependency files
$(OBJS): attach.h port.h scsi_message.h scsipiconf.h version.h scsi_spc.h sd.h cmdhandler.h printf.h scsimsg.h scsipi_base.h siopreg.h device.h scsi_all.h scsipi_debug.h siopvar.h scsi_disk.h scsipi_disk.h sys_queue.h

$(OBJS): Makefile port.h | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) -c $(filter %.c,$^) -o $@

$(OBJSU) $(OBJSM) $(OBJSD): Makefile | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS_TOOLS) -c $(filter %.c,$^) -o $@

$(PROG): $(OBJS) $(ASMOBJS)
	@echo "Building $@ v$(DEVICE_VERSION).$(DEVICE_REVISION) (v$(FULL_VERSION))"
	$(QUIET)$(CC) $(CFLAGS) $(OBJS) $(ASMOBJS) $(LDFLAGS) -o $@
	$(QUIET)$(STRIP) $@
	@LEN="`wc -c < $@| sed 's/^ *//'`"; printf "${yellow}$(PROG) is $${LEN} bytes${end}\n"

$(PROGU): $(OBJSU)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS_TOOLS) $(LDFLAGS_TOOLS) $(OBJSU) -o $@
	$(QUIET)$(STRIP) $@

$(PROGD): $(OBJSD)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS_TOOLS) $(LDFLAGS_TOOLS) $(OBJSD) -o $@
	$(QUIET)$(STRIP) $@

$(OBJDIR)/siop_script.out: siop_script.ss $(SC_ASM)
	@echo Generating $@
	$(QUIET)$(SC_ASM) $(filter %.ss,$^) -p $@

$(OBJDIR)/siop2_script.out: siop2_script.ss $(SC_ASM)
	@echo Generating $@
	$(QUIET)$(SC_ASM) $(filter %.ss,$^) -p $@

$(SC_ASM): ncr53cxxx.c
	@echo Building $@
	$(QUIET)$(HOSTCC) $(HOSTCFLAGS) -o $@ $^

$(OBJDIR)/version.i: Makefile $(OBJDIR)
	$(QUIET)echo 'DEVICE_VERSION EQU $(DEVICE_VERSION)' > $@
	$(QUIET)echo 'DEVICE_REVISION EQU $(DEVICE_REVISION)' >> $@
	$(QUIET)echo 'FULL_VERSION_STR MACRO' >> $@
	$(QUIET)echo '	dc.b "$(FULL_VERSION)"' >> $@
	$(QUIET)echo '	ENDM' >> $@
	$(QUIET)echo 'DATE_STR MACRO' >> $@
	$(QUIET)echo '	dc.b "$(ADATE)"' >> $@
	$(QUIET)echo '	ENDM' >> $@

$(OBJDIR)/%.o: %.S
	@echo Building $@
	$(QUIET)$(VASM) -quiet -m68020 -Fhunk -o $@ $< -I $(NDK_PATH)

%.zx0: %
	@printf "Compressing $< ... "
	$(QUIET)util/zx0wrap $< $@ >/dev/null
	@printf "`wc -c < $<` -> `wc -c < $@` bytes${end}\n"

$(OBJDIR)/rom.o: rom.S reloc.S $(OBJDIR)/version.i Makefile
	@echo Building $@
	$(QUIET)$(VASM) -quiet -m68020 -Fhunk -o $@ $< -I $(OBJDIR) -I $(NDK_PATH) $(TARGETAFLAGS)

$(OBJDIR)/assets.o: assets.S $(PROG) Makefile
	@echo Building $@
	$(QUIET)$(VASM) -quiet -m68020 -Fhunk -o $@ $< -I $(NDK_PATH)

$(OBJDIR)/reloctest.o: reloctest.c
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS_TOOLS) -c $^ -o $@

reloctest: $(OBJDIR)/reloctest.o $(OBJDIR)/reloc.o $(OBJDIR)/assets.o
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS_TOOLS) $(LDFLAGS_TOOLS) $^ -o $@

test: reloctest
	@echo Running relocation test
	$(QUIET)vamos reloctest

$(OBJDIR)/romtool: romtool.c
	@echo Building $@
	$(QUIET)$(HOSTCC) -O2 -Wall $^ -o $@

$(ROM_ND): $(OBJSROM) rom.ld
	@echo Building $@
	$(QUIET)$(VLINK) -Trom.ld -brawbin1 -o $@ $(filter %.o, $^)
	$(QUIET)test `wc -c < $@` -le 32768 && printf "${green}ROM $@ fits in 32k${end}\n" || ( test `wc -c < $@` -gt 65536 && printf "${red}ROM $@ FILE EXCEEDS 64K!${end}\n" || printf "${yellow}ROM $@ fits in 64k${end}\n" )

$(ROM): $(ROM_ND) $(PROG).zx0 $(OBJDIR)/romtool
	@echo Building $@
	$(QUIET)$(OBJDIR)/romtool $(ROM_ND) -o $(ROM) -D $(PROG).zx0

$(ROM_CD): $(ROM) $(CDFS).zx0 $(OBJDIR)/romtool
	@echo Building $@
	$(QUIET)$(OBJDIR)/romtool $(ROM) -o $(ROM_CD) -F $(CDFS).zx0 -T 0x43443031
	$(QUIET)#$(OBJDIR)/romtool $(ROM_CD) --skip -F fat95.zx0 -T 0x46443031

$(KICK): kickmodule.S reloc.S $(OBJDIR)/version.i Makefile $(PROG).zx0
	@echo Building $@
	$(QUIET)$(VASM) -nosym -quiet -m68020 -Fhunkexe -o $@ $< -I $(OBJDIR) -I $(NDK_PATH) $(TARGETAFLAGS)

$(OBJDIR):
	$(QUIET)mkdir -p $@
	$(QUIET)mkdir -p $@/3rdparty/mounter
	$(QUIET)mkdir -p $@/util/a4092flash

util/a4092flash/a4092flash:
	$(QUIET)make -s -C util/a4092flash DEVICE_VERSION=$(DEVICE_VERSION) DEVICE_REVISION=$(DEVICE_REVISION)

clean:
	@echo Cleaning.
	$(QUIET)rm -f $(OBJS) $(OBJSU) $(OBJSM) $(OBJSD) $(OBJSROM) $(OBJSROM_ND) $(OBJSROM_CD) $(OBJDIR)/*.map $(OBJDIR)/*.lst $(SIOP_SCRIPT) $(SC_ASM)
	$(QUIET)rm -f $(PROG).zx0 $(CDFS).zx0
	$(QUIET)rm -f $(OBJDIR)/rom.bin reloctest
	$(QUIET)make -s -C util/a4092flash clean

distclean: clean
	@echo Cleaning really good.
	$(QUIET)rm -f $(PROGU) $(PROGD) *.device *.zx0 *.rom *.kick a4091_*.lha
	$(QUIET)rm -rf $(OBJDIR)

$(OBJDIR)/CDVDFS:
	$(QUIET)$(MAKE) -s -C 3rdparty/CDVDFS/src
	$(QUIET)cp 3rdparty/CDVDFS/src/cdrom-handler $@

kickstart:
	$(QUIET)kick/build_rom.sh $(PROG)

$(ROM_DB):

lha:
	$(QUIET)VER=$(FULL_VERSION) ;\
	echo Creating a4091_$$VER.lha ;\
	mkdir a4091_$$VER ;\
	cp -p *.device *.rom *.kick $(PROGU) $(PROGD)  util/a4092flash/a4092flash $(OBJDIR)/romtool a4091_$$VER ;\
	echo Build $$VER $(DATE) $(TIME) >a4091_$$VER/README.txt ;\
	cat dist.README.txt >>a4091_$$VER/README.txt ;\
	lha -c a4091_$$VER.lha a4091_$$VER >/dev/null ;\
	rm -rf a4091_$$VER

disk:
	@echo "Building Disk image"
	$(QUIET)$(MAKE) -s -C disk

all-targets:
	@echo "Cleaning up first"
	$(QUIET)$(MAKE) -s DEVICE=A4091 distclean
	@echo "Building A4091 debug image"
	$(QUIET)$(MAKE) -s DEVICE=A4091 DEBUG="-DDEBUG -DDEBUG_DEVICE -DDEBUG_SD -DDEBUG_MOUNTER"
	$(QUIET)mv a4091.rom a4091_debug.rom
	$(QUIET)$(MAKE) -s DEVICE=A4091 clean
	@echo "Building A4091 driverless, normal and cdboot image"
	$(QUIET)$(MAKE) -s DEVICE=A4091 a4091_cdfs.rom
	$(QUIET)$(MAKE) -s DEVICE=A4091 clean
	@echo "Building A4092 debug image"
	$(QUIET)$(MAKE) -s DEVICE=A4092 DEBUG="-DDEBUG -DDEBUG_DEVICE -DDEBUG_SD -DDEBUG_MOUNTER"
	$(QUIET)mv a4092.rom a4092_debug.rom
	$(QUIET)$(MAKE) -s DEVICE=A4092 clean
	@echo "Building A4092 driverless, normal and cdboot image"
	$(QUIET)$(MAKE) -s DEVICE=A4092 a4092_cdfs.rom
	$(QUIET)$(MAKE) -s DEVICE=A4092 clean
	@echo "Building scsi710.device and scsi710.kick"
	$(QUIET)$(MAKE) -s DEVICE=A4000T scsi710.kick
	$(QUIET)$(MAKE) -s DEVICE=A4000T clean
	@echo "Building scsi770.device and scsi770.kick"
	$(QUIET)$(MAKE) -s DEVICE=A4000T770 scsi770.kick
	$(QUIET)$(MAKE) -s DEVICE=A4000T770 clean
	@echo "Building utilities"
	$(QUIET)$(MAKE) -s ncr7xx
	$(QUIET)$(MAKE) -s a4091d
	$(QUIET)$(MAKE) -s util/a4092flash/a4092flash
	@echo "Building Disk image"
	$(QUIET)$(MAKE) -s disk
	$(QUIET)$(MAKE) -s DEVICE=A4091 lha

.PHONY: verbose all $(OBJDIR)/CDVDFS disk
