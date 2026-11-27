.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
include mk/build-config.mk

NOW     := $(shell date '+%Y-%m-%d %H:%M:%S')
DATE    := $(firstword $(NOW))
TIME    := $(lastword $(NOW))
ADATE   := $(shell date '+%-d.%-m.%Y')
# FULL_VERSION is 42.xx-yy-dirty
FULL_VERSION ?= $(shell git describe --tags --dirty | sed -r 's/^(release_|v)//')
FULL_VERSION := $(FULL_VERSION)
DEVICE_VERSION := $(shell echo $(FULL_VERSION) | cut -f1 -d\.)
DEVICE_REVISION := $(shell echo $(FULL_VERSION) | cut -f2 -d\.|cut -f1 -d\-)

# Default to NCR53C770 driver:

#DEVICE := A4000T
#DEVICE := A4000T770
#DEVICE := A4092
DEVICE ?= A4091
SIOP_770_USE_4BIT_OFFSET ?= 1

ifeq ($(DEVICE),A4091)
TARGET  := NCR53C710
TARGETCFLAGS := -DDRIVER_A4091 -DNCR53C710=1 -DARCH_710 -DDEVNAME="a4091" -DNO_CONFIGDEV=0 -DHAVE_ROM=1
TARGETAFLAGS := -DNCR53C710=1 -DDRIVER_A4091=1
override DEVNAME := a4091
HAVE_ROM=y
else ifeq ($(DEVICE),A4092)
TARGET  := NCR53C710
TARGETCFLAGS := -DDRIVER_A4092 -DNCR53C710=1 -DARCH_710 -DDEVNAME="a4092" -DNO_CONFIGDEV=0 -DHAVE_ROM=1 -DFLASH_SPI=1 # -DFLASH_PARALLEL=1
TARGETAFLAGS := -DNCR53C710=1 -DDRIVER_A4092=1
override DEVNAME := a4092
HAVE_ROM=y
else ifeq ($(DEVICE),A4770)
TARGET  := NCR53C770
TARGETCFLAGS := -DDRIVER_A4770 -DNCR53C770=1 -DARCH_770 -DDEVNAME="a4770" -DNO_CONFIGDEV=0 -DHAVE_ROM=1 -DFLASH_SPI=1 -DSIOP_770_USE_4BIT_OFFSET=$(SIOP_770_USE_4BIT_OFFSET)
TARGETAFLAGS := -DNCR53C770=1 -DDRIVER_A4770=1
override DEVNAME := a4770
HAVE_ROM=y
else ifeq ($(DEVICE),A4000T)
TARGET  := NCR53C710
TARGETCFLAGS := -DDRIVER_A4000T -DNCR53C710=1 -DARCH_710 -DDEVNAME="scsi710" -DNO_CONFIGDEV=1 -DHAVE_ROM=0 -DOEM_KICKMODULE=1
TARGETAFLAGS := -DNCR53C710=1
override DEVNAME := scsi710
HAVE_ROM=n
else ifeq ($(DEVICE),A4000T770)
TARGET  := NCR53C770
TARGETCFLAGS := -DDRIVER_A4000T770 -DNCR53C770=1 -DARCH_770 -DDEVNAME="scsi770" -DNO_CONFIGDEV=1 -DHAVE_ROM=0 -DSIOP_770_USE_4BIT_OFFSET=$(SIOP_770_USE_4BIT_OFFSET) -DOEM_KICKMODULE=1
TARGETAFLAGS := -DNCR53C770=1
override DEVNAME := scsi770
HAVE_ROM=n
else
$(error Unknown build target! Please set DEVICE to A4091, A4092, A4770, A4000T or A4000T770.)
endif

OBJDIR  ?= objs-$(DEVICE)

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
ifneq (,$(filter $(DEVNAME),a4092 a4770))
SRCS    += util/a4092flash/flash.c util/a4092flash/nvram_flash.c util/a4092flash/spi.c mfg.c
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
BOARD_DEBUGGER := $(DEVNAME).a4091d
FLASH_DIR := util/a4092flash
FLASH_TOOLS := $(FLASH_DIR)/a4092flash $(FLASH_DIR)/mfgtool
TOOLS := $(PROGU) $(PROGD) $(FLASH_TOOLS)
DISK_FILES := $(PROG) $(BOARD_DEBUGGER)
ifneq (,$(filter $(DEVNAME),a4092 a4770))
DISK_FILES += $(ROM_CD)
endif

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
CFLAGS  += -Wall -Wextra -Wno-pointer-sign -Wshadow -Wcast-align -Wcast-qual
CFLAGS  += -Wstrict-prototypes -Wmissing-prototypes -fanalyzer
CFLAGS += -mcpu=68060
CFLAGS += -DDEVICE_VERSION=$(DEVICE_VERSION)
CFLAGS += -DDEVICE_REVISION=$(DEVICE_REVISION)
CFLAGS += -DFULL_VERSION="$(FULL_VERSION)"

CFLAGS_TOOLS := -Wall -Wextra -Wno-pointer-sign -fomit-frame-pointer -Os -mcpu=68060
CFLAGS_TOOLS += -DAMIGA_DATE=\"$(ADATE)\"

LDFLAGS_COMMON = -Wl,-Map=$(OBJDIR)/$(@F).map -Wa,-a > $(OBJDIR)/$(@F).lst
LDFLAGS        = -nostartfiles -nostdlib -Tdevice.ld -ldebug -lgcc -lc -lamiga -ramiga-dev $(LDFLAGS_COMMON)
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
freshsubs:=$(shell git submodule update --init --recursive $(quiet_errors))
endif

# Autodetect which CDFileSystem is available if any. This mechanism
# lets you build with an AmigaOS CDFileSystem if you drop the file
# into the source tree. Per default, we are building with the
# ODFileSystem ROM profile from 3rdparty/ODFileSystem.

ifneq (,$(wildcard BootCDFileSystem))
CDFS=BootCDFileSystem
else
ifneq (,$(wildcard CDFileSystem))
CDFS=CDFileSystem
else
CDFS=$(OBJDIR)/ODFileSystem
endif
endif

DEBUG_CDFS ?= 0
ifeq ($(DEBUG_CDFS),1)
ODFS_ROM_TARGET=rom-test
ODFS_ROM_BUILD_DIR=build/amiga-rom-test
else
ODFS_ROM_TARGET=rom
ODFS_ROM_BUILD_DIR=build/amiga-rom
endif

ifeq ($(HAVE_ROM),y)
ROMS:=$(ROM) $(ROM_CD)
else
# Build relocatable kickstart module if we don't have to build a ROM image
ROMS:=$(KICK)
endif

all: $(PROG) $(ROMS) $(TOOLS)

# BUILD_TIME is the invocation time, not a build option. Keep it available
# when compiling without invalidating every object on every invocation.
BUILD_CONFIG := $(OBJDIR)/build.config
$(BUILD_CONFIG): CONFIG_TEXT := $(CC) $(filter-out -DBUILD_TIME=%,$(CFLAGS)) $(CFLAGS_TOOLS) \
	$(LDFLAGS) $(LDFLAGS_TOOLS) $(STRIP) $(VASM) $(VLINK) $(TARGETAFLAGS) $(NDK_PATH) $(ADATE) $(FULL_VERSION)
$(BUILD_CONFIG): FORCE | $(OBJDIR)
	$(write-config)

$(OBJDIR)/version.o: $(filter-out $(OBJDIR)/version.o,$(OBJS) $(ASMOBJS))
$(OBJDIR)/siop.o: $(OBJDIR)/siop_script.out
$(OBJDIR)/siop2.o: $(OBJDIR)/siop2_script.out
$(OBJDIR)/siop.o $(OBJDIR)/siop2.o: CFLAGS += -I$(OBJDIR)
$(OBJDIR)/a4091d.o: CFLAGS_TOOLS += -D_KERNEL -DPORT_AMIGA $(TARGETCFLAGS) -Wno-format
$(OBJDIR)/ncr7xx.o: CFLAGS_TOOLS += -Wno-format

DEPFLAGS = -MMD -MP -MF $(@:.o=.d) -MT $@
$(OBJS): $(OBJDIR)/%.o: %.c Makefile $(BUILD_CONFIG) | $(OBJDIR)
	$(QUIET)mkdir -p $(@D)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJSU) $(OBJSD) $(OBJDIR)/reloctest.o: $(OBJDIR)/%.o: %.c Makefile $(BUILD_CONFIG) | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS_TOOLS) $(DEPFLAGS) -c $< -o $@

-include $(OBJS:.o=.d) $(OBJSU:.o=.d) $(OBJSD:.o=.d) $(OBJDIR)/reloctest.d

$(PROG): $(OBJS) $(ASMOBJS) device.ld
	@echo "Building $@ v$(DEVICE_VERSION).$(DEVICE_REVISION) (v$(FULL_VERSION))"
	$(QUIET)$(CC) $(CFLAGS) $(OBJS) $(ASMOBJS) $(LDFLAGS) -o $@
	$(QUIET)$(STRIP) $@
	@LEN="`wc -c < $@| sed 's/^ *//'`"; printf "${yellow}$(PROG) is $${LEN} bytes${end}\n"

$(OBJDIR)/$(PROGU): $(OBJSU)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS_TOOLS) $(LDFLAGS_TOOLS) $(OBJSU) -o $@
	$(QUIET)$(STRIP) $@

$(BOARD_DEBUGGER): $(OBJSD)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS_TOOLS) $(LDFLAGS_TOOLS) $(OBJSD) -o $@
	$(QUIET)$(STRIP) $@

# Keep the traditional CLI name while caching each board's executable.
$(PROGD): $(BOARD_DEBUGGER) FORCE
	$(QUIET)cmp -s $< $@ || cp $< $@

$(PROGU): $(OBJDIR)/$(PROGU) FORCE
	$(QUIET)cmp -s $< $@ || cp $< $@

$(OBJDIR)/siop_script.out: siop_script.ss $(SC_ASM)
	@echo Generating $@
	$(QUIET)$(SC_ASM) $(filter %.ss,$^) -p $@

$(OBJDIR)/siop2_script.out: siop2_script.ss $(SC_ASM)
	@echo Generating $@
	$(QUIET)$(SC_ASM) $(filter %.ss,$^) -p $@

$(SC_ASM): ncr53cxxx.c | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(HOSTCC) $(HOSTCFLAGS) -o $@ $^

$(OBJDIR)/version.i: Makefile $(BUILD_CONFIG) | $(OBJDIR)
	$(QUIET)echo 'DEVICE_VERSION EQU $(DEVICE_VERSION)' > $@
	$(QUIET)echo 'DEVICE_REVISION EQU $(DEVICE_REVISION)' >> $@
	$(QUIET)echo 'FULL_VERSION_STR MACRO' >> $@
	$(QUIET)echo '	dc.b "$(FULL_VERSION)"' >> $@
	$(QUIET)echo '	ENDM' >> $@
	$(QUIET)echo 'DATE_STR MACRO' >> $@
	$(QUIET)echo '	dc.b "$(ADATE)"' >> $@
	$(QUIET)echo '	ENDM' >> $@

DEPACKER := 3rdparty/salvador/asm/68000/unzx0_68000.S
$(OBJDIR)/reloc.o: $(DEPACKER) $(BUILD_CONFIG)

$(OBJDIR)/%.o: %.S Makefile | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(VASM) -quiet -m68020 -Fhunk -o $@ $< -I $(NDK_PATH)

SALVADOR_DIR := 3rdparty/salvador
SALVADOR := $(OBJDIR)/salvador-$(shell uname -s)-$(shell uname -m)
SALVADOR_INPUTS := $(wildcard $(SALVADOR_DIR)/src/*.[ch] \
	$(SALVADOR_DIR)/src/libdivsufsort/lib/*.[ch] $(SALVADOR_DIR)/src/libdivsufsort/include/*.h)
$(SALVADOR): $(SALVADOR_DIR)/Makefile $(SALVADOR_INPUTS) | $(OBJDIR)
	$(QUIET)$(MAKE) -B -s -C $(SALVADOR_DIR) CC="$(HOSTCC)" \
		OBJDIR="$(abspath $@).objs" APP="$(abspath $@)" all

%.zx0: % util/zx0wrap $(SALVADOR)
	@printf "Compressing $< ... "
	$(QUIET)util/zx0wrap $< $@ "$(abspath $(SALVADOR))" >/dev/null
	@printf "`wc -c < $<` -> `wc -c < $@` bytes${end}\n"

$(OBJDIR)/rom.o: rom.S reloc.S $(DEPACKER) $(OBJDIR)/version.i Makefile
	@echo Building $@
	$(QUIET)$(VASM) -quiet -m68020 -Fhunk -o $@ $< -I $(OBJDIR) -I $(NDK_PATH) $(TARGETAFLAGS)

$(OBJDIR)/test-driver.device: $(PROG) | $(OBJDIR)
	$(QUIET)cp $< $@

$(OBJDIR)/assets.o: assets.S $(OBJDIR)/test-driver.device Makefile | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(VASM) -quiet -m68020 -Fhunk -o $@ $< -I $(OBJDIR) -I $(NDK_PATH)

$(OBJDIR)/reloctest: $(OBJDIR)/reloctest.o $(OBJDIR)/reloc.o $(OBJDIR)/assets.o
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS_TOOLS) $(LDFLAGS_TOOLS) $^ -o $@

reloctest: $(OBJDIR)/reloctest FORCE
	$(QUIET)cmp -s $< $@ || cp $< $@

test: reloctest
	@echo Running relocation test
	$(QUIET)vamos -C 68020 reloctest

$(OBJDIR)/romtool: romtool.c | $(OBJDIR)
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

$(KICK): kickmodule.S reloc.S $(DEPACKER) $(OBJDIR)/version.i Makefile $(PROG).zx0
	@echo Building $@
	$(QUIET)$(VASM) -nosym -quiet -m68020 -Fhunkexe -o $@ $< -I $(OBJDIR) -I $(NDK_PATH) $(TARGETAFLAGS)

scsi_assets.kick: kickmodule.S $(OBJDIR)/version.i Makefile oem.bin
	@echo Building $@
	$(QUIET)$(VASM) -nosym -quiet -m68020 -Fhunkexe -o $@ $< -I $(OBJDIR) -I $(NDK_PATH) -DOEM_KICKMODULE=1

$(OBJDIR):
	$(QUIET)mkdir -p $@

# The submake owns the dependencies and builds both outputs in one pass.
$(FLASH_TOOLS): flash-tools ;
flash-tools:
	$(QUIET)$(MAKE) -s -C $(FLASH_DIR) DEVICE_VERSION=$(DEVICE_VERSION) DEVICE_REVISION=$(DEVICE_REVISION) all

clean:
	@echo Cleaning.
	$(QUIET)rm -f $(OBJS) $(OBJSU) $(OBJSD) $(OBJS:.o=.d) $(OBJSU:.o=.d) $(OBJSD:.o=.d) $(ASMOBJS) $(OBJSROM) $(OBJDIR)/*.map $(OBJDIR)/*.lst $(SIOP_SCRIPT) $(SC_ASM)
	$(QUIET)rm -f $(PROG).zx0 $(CDFS).zx0
	$(QUIET)rm -f $(OBJDIR)/rom.bin reloctest
	$(QUIET)$(MAKE) -s -C $(FLASH_DIR) clean

distclean: clean
	@echo Cleaning really good.
	$(QUIET)$(MAKE) -s -C 3rdparty/ODFileSystem clean
	$(QUIET)rm -f $(PROGU) $(PROGD) *.device *.a4091d *.zx0 *.rom *.kick scsi_assets.kick a4091_*.lha
	$(QUIET)rm -rf $(OBJDIR) $(DEBUG_DIR)

$(OBJDIR)/ODFileSystem: FORCE | $(OBJDIR)
	$(QUIET)$(MAKE) -s -C 3rdparty/ODFileSystem \
		CC="$(CC)" STRIP="$(STRIP)" HOSTCC="$(HOSTCC)" \
		NDK_PATH="$(NDK_PATH)" AMIGA_DATE="$(ADATE)" $(ODFS_ROM_TARGET)
	$(QUIET)src=3rdparty/ODFileSystem/$(ODFS_ROM_BUILD_DIR)/ODFileSystem; cmp -s "$$src" $@ || cp "$$src" $@

ifeq ($(HAVE_ROM),n)
kickstart: $(KICK)
endif
kickstart:
	$(QUIET)if [ -f oem.bin ]; then $(MAKE) -s scsi_assets.kick; fi
	$(QUIET)kick/build_rom.sh $(PROG)

# Build the debug driver and its ROM in a separate directory. Packaging a
# debug image must not replace the normal driver used by the floppy.
DEBUG_DIR := $(OBJDIR)-debug
$(ROM_DB): FORCE
	$(QUIET)$(MAKE) OBJDIR=$(DEBUG_DIR) PROG=$(DEBUG_DIR)/$(DEVNAME).device \
		ROM_ND=$(DEBUG_DIR)/$(DEVNAME)_nodriver.rom ROM=$(DEBUG_DIR)/$(DEVNAME).rom \
		DEBUG="-DDEBUG -DDEBUG_DEVICE -DDEBUG_SD -DDEBUG_MOUNTER" $(DEBUG_DIR)/$(DEVNAME).rom
	$(QUIET)cmp -s $(DEBUG_DIR)/$(DEVNAME).rom $@ || cp $(DEBUG_DIR)/$(DEVNAME).rom $@

# Each board submake owns its driver, ROMs and matching debugger.
ROM_BOARDS := A4091 A4092 A4770
KICK_BOARDS := A4000T A4000T770
release-board: $(PROG) $(ROMS) $(BOARD_DEBUGGER) $(if $(filter y,$(HAVE_ROM)),$(ROM_DB))
release-binaries:
	$(QUIET)set -e; for board in $(ROM_BOARDS) $(KICK_BOARDS); do \
		$(MAKE) DEVICE=$$board OBJDIR=objs-$$board release-board; \
	done

RELEASE_DRIVERS := a4091.device a4092.device a4770.device scsi710.device scsi770.device
RELEASE_ROMS := $(foreach dev,a4091 a4092 a4770,$(dev).rom $(dev)_debug.rom $(dev)_cdfs.rom $(dev)_nodriver.rom)
RELEASE_KICKS := scsi710.kick scsi770.kick
lha: release-binaries
	$(QUIET)$(MAKE) DEVICE=A4091 OBJDIR=objs-A4091 archive
OPTIONAL_KICK := $(if $(wildcard oem.bin),scsi_assets.kick)
archive: $(OPTIONAL_KICK) $(PROGU) $(PROGD) $(FLASH_TOOLS) $(OBJDIR)/romtool dist.README.txt
	$(QUIET)set -e; name="a4091_$(FULL_VERSION)"; \
		stage=$$(mktemp -d "./.lha-XXXXXX"); trap 'rm -rf "$$stage"' EXIT; \
		mkdir "$$stage/$$name"; \
		cp -p $(RELEASE_DRIVERS) $(RELEASE_ROMS) $(RELEASE_KICKS) $(OPTIONAL_KICK) \
			$(PROGU) $(PROGD) $(FLASH_DIR)/a4092flash $(OBJDIR)/romtool "$$stage/$$name/"; \
		echo "Build $(FULL_VERSION) $(DATE) $(TIME)" > "$$stage/$$name/README.txt"; \
		cat dist.README.txt >> "$$stage/$$name/README.txt"; \
		(cd "$$stage" && lha -c "$$name.lha" "$$name" >/dev/null); \
		mv -f "$$stage/$$name.lha" "$$name.lha"

# The disk script also emits variants for the board binaries already here.
# Refresh those inputs under this build's settings before copying any files.
DISK_BOARDS := $(if $(wildcard a4091.device),A4091) \
	$(if $(and $(wildcard a4092.device),$(wildcard a4092_cdfs.rom)),A4092) \
	$(if $(and $(wildcard a4770.device),$(wildcard a4770_cdfs.rom)),A4770) \
	$(if $(wildcard scsi710.device),A4000T) $(if $(wildcard scsi770.device),A4000T770)
disk-board: $(DISK_FILES)
disk: disk-board $(TOOLS)
	$(QUIET)set -e; for board in $(filter-out $(DEVICE),$(DISK_BOARDS)); do \
		$(MAKE) DEVICE=$$board OBJDIR=objs-$$board disk-board; \
	done
	$(QUIET)$(MAKE) -s -C disk DEVICE=$(DEVICE) DEVNAME=$(DEVNAME) FULL_VERSION="$(FULL_VERSION)" ADATE="$(ADATE)" image

all-targets: lha
	$(QUIET)$(MAKE) DEVICE=A4091 OBJDIR=objs-A4091 disk

.PHONY: verbose all clean distclean test kickstart flash-tools release-board release-binaries
.PHONY: lha archive disk-board disk all-targets
