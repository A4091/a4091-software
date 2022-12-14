NOW     := $(shell date '+%Y-%m-%d %H:%M:%S')
DATE    := $(firstword $(NOW))
TIME    := $(lastword $(NOW))

OBJDIR  := objs
ROM	:= a4091.rom
PROG	:= a4091.device
PROGU	:= a4091
PROGD	:= a4091d
SRCS    := device.c version.c siop.c port.c attach.c cmdhandler.c printf.c
SRCS    += sd.c scsipi_base.c scsiconf.c scsimsg.c mounter.c bootmenu.c
SRCS    += romfile.c battmem.c
ASMSRCS := reloc.S
SRCSU   := a4091.c
SRCSD   := a4091d.c
OBJS    := $(SRCS:%.c=$(OBJDIR)/%.o)
OBJSD   := $(SRCSD:%.c=$(OBJDIR)/%.o)
OBJSU   := $(SRCSU:%.c=$(OBJDIR)/%.o)
ASMOBJS := $(ASMSRCS:%.S=$(OBJDIR)/%.o)

HOSTCC  ?= cc
CC      := m68k-amigaos-gcc
STRIP   := m68k-amigaos-strip
VLINK   := vlink
VASM    := vasmm68k_mot

# Find an appropriate set of NDK includes
NDK_PATHS := /opt/amiga/m68k-amigaos/ndk-include
NDK_PATHS += /opt/amiga-2021.05/m68k-amigaos/ndk-include
NDK_PATH  := $(firstword $(wildcard $(NDK_PATHS)))

# CFLAGS for a4091.device
#
CFLAGS  := -DBUILD_DATE=\"$(DATE)\" -DBUILD_TIME=\"$(TIME)\"
CFLAGS  += -D_KERNEL -DPORT_AMIGA
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

CFLAGS  += -DENABLE_SEEK  # Not needed for modern drives (~500 bytes)
CFLAGS  += -Os -fomit-frame-pointer -noixemul
#CFLAGS  += -fbaserel -resident -DUSING_BASEREL
CFLAGS  += -msmall-code
CFLAGS  += -Wall -Wno-pointer-sign -Wno-strict-aliasing
CFLAGS += -mcpu=68060

CFLAGS_TOOLS := -Wall -Wno-pointer-sign -fomit-frame-pointer -Os -mcpu=68060

# Enable to put the original Commodore driver into the ROM
# (You will have to extract it yourself)
#ROMDRIVER := -DCOMMODORE_DEVICE=1
#ROMDRIVER := -DNO_DEVICE=1
# Adding -DNO_DEVICE=1 by itself will cause a ROM image to be built
# without any driver. This is useful for board testing with "a4091 -t"

#CDFILESYSTEM := -DCDFS=1

LDFLAGS_COMMON = -Wl,-Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst
LDFLAGS        = -nostartfiles -nostdlib -ldebug -lgcc -lc -lamiga -ramiga-dev $(LDFLAGS_COMMON)
LDFLAGS_TOOLS  = -mcrt=clib2 -lgcc -lc -lamiga $(LDFLAGS_COMMON)

#CFLAGS  += -g
#LDFLAGS += -g

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
SIOP_SCRIPT := $(OBJDIR)/siop_script.out

red=\033[1;31m
green=\033[1;32m
yellow=\033[1;33m
end=\033[0m

ifeq (, $(shell which $(CC) 2>/dev/null ))
$(error "No $(CC) in PATH: maybe do PATH=$$PATH:/opt/amiga/bin")
endif

all: $(PROG) $(PROGU) $(PROGD) $(ROM)

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(OBJDIR)/%,$(filter-out $(OBJDIR)/%,$(basename $(1)).o)) $(filter $(OBJDIR)/%,$(basename $(1)).o): $(1)
endef
$(foreach SRCFILE,$(SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE))))
$(foreach SRCFILE,$(SRCSU),$(eval $(call DEPEND_SRC,$(SRCFILE))))
$(foreach SRCFILE,$(SRCSD),$(eval $(call DEPEND_SRC,$(SRCFILE))))

$(OBJDIR)/version.o: version.h $(filter-out $(OBJDIR)/version.o, $(OBJS) $(ASMOBJS))
$(OBJDIR)/siop.o: $(SIOP_SCRIPT)
$(OBJDIR)/siop.o:: CFLAGS += -I$(OBJDIR)
$(OBJDIR)/a4091d.o:: CFLAGS_TOOLS += -D_KERNEL -DPORT_AMIGA

# XXX: Need to generate real dependency files
$(OBJS): attach.h port.h scsi_message.h scsipiconf.h version.h port_bsd.h scsi_spc.h sd.h cmdhandler.h printf.h scsimsg.h scsipi_base.h siopreg.h device.h scsi_all.h scsipi_debug.h siopvar.h scsi_disk.h scsipi_disk.h sys_queue.h

$(OBJS): Makefile port.h | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) -c $(filter %.c,$^) -o $@

$(OBJSU) $(OBJSM) $(OBJSD): Makefile | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS_TOOLS) -c $(filter %.c,$^) -o $@

$(PROG): $(OBJS) $(ASMOBJS)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) $(OBJS) $(ASMOBJS) $(LDFLAGS) -o $@
	$(QUIET)$(STRIP) $@

$(PROGU): $(OBJSU)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS_TOOLS) $(LDFLAGS_TOOLS) $(OBJSU) -o $@

$(PROGD): $(OBJSD)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS_TOOLS) $(LDFLAGS_TOOLS) $(OBJSD) -o $@

$(SIOP_SCRIPT): siop_script.ss $(SC_ASM)
	@echo Generating $@
	$(QUIET)$(SC_ASM) $(filter %.ss,$^) -p $@

$(SC_ASM): ncr53cxxx.c
	@echo Building $@
	$(QUIET)$(HOSTCC) $(HOSTCFLAGS) -o $@ $^

$(OBJDIR)/rom.o: rom.S reloc.S Makefile
	@echo Building $@
	$(QUIET)$(VASM) -quiet -m68020 -Fhunk -o $@ $< -I $(NDK_PATH) $(ROMDRIVER)

$(OBJDIR)/reloc.o: reloc.S
	@echo Building $@
	$(QUIET)$(VASM) -quiet -m68020 -Fhunk -o $@ $< -I $(NDK_PATH)

$(OBJDIR)/assets.o: assets.S $(PROG) Makefile
	@echo Building $@
	$(QUIET)$(VASM) -quiet -m68020 -Fhunk -o $@ $< -I $(NDK_PATH) $(ROMDRIVER) $(CDFILESYSTEM)

$(OBJDIR)/reloctest.o: reloctest.c
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS_TOOLS) -c $^ -o $@

reloctest: $(OBJDIR)/reloctest.o $(OBJDIR)/reloc.o $(OBJDIR)/assets.o
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS_TOOLS) $(LDFLAGS_TOOLS) $^ -o $@

test: reloctest
	@echo Running relocation test
	$(QUIET)vamos reloctest

a4091.rom: $(OBJDIR)/rom.o $(OBJDIR)/assets.o rom.ld
	@echo Building $@
	$(QUIET)$(VLINK) -Trom.ld -brawbin1 -o $@ $< $(OBJDIR)/assets.o
	@printf "${yellow}$(PROG) is $(shell echo `wc -c < "$(PROG)"`) bytes${end}\n"
	$(QUIET)test `wc -c < $@` -le 32768 && printf "${green}ROM fits in 32k${end}\n" || ( test `wc -c < $@` -gt 65536 && printf "${red}ROM FILE EXCEEDS 64K!${end}\n" || printf "${yellow}ROM fits in 64k${end}\n" )

$(OBJDIR):
	mkdir -p $@

clean:
	@echo Cleaning
	$(QUIET)rm -f $(OBJS) $(OBJSU) $(OBJSM) $(OBJSD) $(OBJDIR)/*.map $(OBJDIR)/*.lst $(SIOP_SCRIPT) $(SC_ASM)
	$(QUIET)rm -f $(OBJDIR)/rom.o $(OBJDIR)/rom.bin reloctest

distclean: clean
	@echo $@
	$(QUIET)rm -f $(PROG) $(PROGU) $(PROGD) $(ROM)
	$(QUIET)rm -r $(OBJDIR)

lha: all
	@VER=$$(awk '/define DEVICE_/{if (V != "") print V"."$$NF; else V=$$NF}' version.h) ;\
	echo Creating a4091_$$VER.lha ;\
	mkdir a4091_$$VER ;\
	cp -p $(PROG) $(PROGU) $(PROGD) $(ROM) a4091_$$VER ;\
	echo Build $$VER $(DATE) $(TIME) >a4091_$$VER/README.txt ;\
	cat dist.README.txt >>a4091_$$VER/README.txt ;\
	lha -c a4091_$$VER.lha a4091_$$VER >/dev/null ;\
	rm -rf a4091_$$VER

.PHONY: verbose all
