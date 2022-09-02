NOW     := $(shell date '+%Y-%m-%d %H:%M:%S')
DATE    := $(firstword $(NOW))
TIME    := $(lastword $(NOW))

ROM	:= a4091.rom
PROG	:= a4091.device
PROGU	:= a4091
PROGM	:= mounter
PROGD	:= a4091d
OBJDIR  := objs
SRCS    := device.c version.c siop.c port.c attach.c cmdhandler.c printf.c
SRCS    += sd.c scsipi_base.c scsiconf.c scsimsg.c rdb_partitions.c bootmenu.c
SRCSU   := a4091.c
SRCSD   := a4091d.c
SRCSM   := mounter.c
OBJS    := $(SRCS:%.c=$(OBJDIR)/%.o)
OBJSD   := $(SRCSD:%.c=$(OBJDIR)/%.o)
OBJSU   := $(SRCSU:%.c=$(OBJDIR)/%.o)
OBJSM   := $(SRCSM:%.c=$(OBJDIR)/%.o)
CC	:= m68k-amigaos-gcc
STRIP   := m68k-amigaos-strip
VLINK   := vlink
VASM    := vasmm68k_mot
NDK_PATHS := /opt/amiga/m68k-amigaos/ndk-include
NDK_PATHS += /opt/amiga-2021.05/m68k-amigaos/ndk-include
NDK_PATH  := $(firstword $(wildcard $(NDK_PATHS)))
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
#CFLAGS  += -DDEBUG_RDB         # Debug rdb_partitions.c
#CFLAGS  += -DDEBUG_BOOTMENU    # Debug bootmenu.c
#CFLAGS2 += -DDEBUG_MOUNTER     # Debug mounter.c # Makes more sense when integrated
#CFLAGS  += -DNO_SERIAL_OUTPUT   # Turn off serial debugging for the whole driver

CFLAGS  += -DENABLE_SEEK  # Not needed for modern drives (~500 bytes)
#CFLAGS  += -mhard-float
CFLAGS  += -Wall -Wno-pointer-sign -fomit-frame-pointer -Os
CFLAGS  += -Wno-strict-aliasing
CFLAGS2 := -Wall -Wno-pointer-sign -fomit-frame-pointer -Os

# Enable to put the original Commodore driver into the ROM
# (You will have to extract it yourself)
#ROMDRIVER := -DCOMMODORE_DEVICE=1
#ROMDRIVER := -DNO_DEVICE=1

LDFLAGS = -Os -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -fomit-frame-pointer -nostartfiles -ldebug -nostdlib -lgcc -lc -lamiga -ramiga-dev
LDFLAGS2 = -Os -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -fomit-frame-pointer -mcrt=clib2
LDFLAGS3 = -Os -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -fomit-frame-pointer -mcrt=clib2 -lgcc -lc -lamiga

#CFLAGS  += -g
#LDFLAGS += -g

# Add link-time optimization (must also have optimization specified in LDFLAGS)
# -flto

# These don't work when compiled as a .device
#CFLAGS  += -fbaserel
#LDFLAGS += -fbaserel

#CFLAGS  += -fbbb=-
#LDFLAGS += -fbbb=-

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

all: $(PROG) $(PROGU) $(PROGM) $(PROGD) $(ROM)

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(OBJDIR)/%,$(filter-out $(OBJDIR)/%,$(basename $(1)).o)) $(filter $(OBJDIR)/%,$(basename $(1)).o): $(1)
endef
$(foreach SRCFILE,$(SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE))))
$(foreach SRCFILE,$(SRCSU),$(eval $(call DEPEND_SRC,$(SRCFILE))))
$(foreach SRCFILE,$(SRCSD),$(eval $(call DEPEND_SRC,$(SRCFILE))))
$(foreach SRCFILE,$(SRCSM),$(eval $(call DEPEND_SRC,$(SRCFILE))))

$(OBJDIR)/version.o: version.h $(filter-out $(OBJDIR)/version.o, $(OBJS))
$(OBJDIR)/siop.o: $(SIOP_SCRIPT)
$(OBJDIR)/siop.o:: CFLAGS += -I$(OBJDIR)
$(OBJDIR)/a4091d.o:: CFLAGS2 += -D_KERNEL -DPORT_AMIGA
$(OBJDIR)/attach.o: device.h

# XXX: Need to generate real dependency files
$(OBJS): attach.h port.h scsi_message.h scsipiconf.h version.h port_bsd.h scsi_spc.h sd.h cmdhandler.h printf.h scsipi_base.h siopreg.h device.h scsi_all.h scsipi_debug.h siopvar.h scsi_disk.h scsipi_disk.h sys_queue.h

$(OBJS): Makefile port.h | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) -c $(filter %.c,$^) -o $@

$(OBJSU) $(OBJSM) $(OBJSD): Makefile | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS2) -c $(filter %.c,$^) -o $@

$(PROG): $(OBJS)
	@echo Building $@
	$(QUIET)$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(QUIET)$(STRIP) $@
	@printf "${yellow}$@ is $(shell echo `wc -c < "$@"`) bytes${end}\n"

$(PROGU): $(OBJSU)
	@echo Building $@
	$(QUIET)$(CC) $(OBJSU) $(LDFLAGS2) -o $@

$(PROGM): $(OBJSM)
	@echo Building $@
	$(QUIET)$(CC) $(OBJSM) $(LDFLAGS3) -o $@

$(PROGD): $(OBJSD)
	@echo Building $@
	$(QUIET)$(CC) $(filter %.o,$^) $(LDFLAGS3) -o $@

$(SIOP_SCRIPT): siop_script.ss $(SC_ASM)
	@echo Building $@
	$(QUIET)$(SC_ASM) $(filter %.ss,$^) -p $@

$(SC_ASM): ncr53cxxx.c
	@echo Building $@
	$(QUIET)cc -o $@ $^

$(OBJDIR)/rom.o: rom.S reloc.S $(PROG)
	@echo Building $@
	$(QUIET)$(VASM) -quiet -m68020 -Fhunk -o $@ $< -I $(NDK_PATH) $(ROMDRIVER)

$(OBJDIR)/reloc.o: reloc.S $(PROG)
	$(QUIET)$(VASM) -quiet -m68020 -Fhunk -o $@ $< -I $(NDK_PATH) -DUSERLAND=1 $(ROMDRIVER)

$(OBJDIR)/reloctest.o: reloctest.c
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS2) -c $^ -o $@

reloctest: $(OBJDIR)/reloctest.o $(OBJDIR)/reloc.o
	@echo Building $@
	$(QUIET)$(CC) $^ $(LDFLAGS2) -o $@

test: reloctest
	@echo Running relocation test
	$(QUIET)vamos reloctest

a4091.rom: $(OBJDIR)/rom.o rom.ld
	@echo Building $@
	$(QUIET)$(VLINK) -Trom.ld -brawbin1 -o $@ $<
	$(QUIET)test `wc -c <$<` -gt 32768 && printf "${red}ROM FILE EXCEEDS 32K!${end}\n" || printf "${green}ROM fits in 32k${end}\n"

$(OBJDIR):
	mkdir -p $@

clean:
	@echo Cleaning
	$(QUIET)rm -f $(OBJS) $(OBJSU) $(OBJSM) $(OBJDIR)/*.map $(OBJDIR)/*.lst $(SIOP_SCRIPT) $(SC_ASM)
	$(QUIET)rm -f $(OBJDIR)/rom.o $(OBJDIR)/rom.bin reloctest

distclean: clean
	@echo $@
	$(QUIET)rm -f $(PROG) $(PROG2) $(ROM)
	$(QUIET)rm -r $(OBJDIR)

.PHONY: verbose
