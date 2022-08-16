NOW     := $(shell date)
DATE    := $(shell date -d '$(NOW)' '+%Y-%m-%d')
TIME    := $(shell date -d '$(NOW)' '+%H:%M:%S')

ROM	:= a4091.rom
PROG	:= a4091.device
PROG2	:= mounter
OBJDIR  := objs
SRCS    := device.c version.c siop.c port.c attach.c cmdhandler.c printf.c
SRCS    += sd.c scsipi_base.c scsiconf.c
SRCS2   := mounter.c
OBJS    := $(SRCS:%.c=$(OBJDIR)/%.o)
OBJS2   := $(SRCS2:%.c=$(OBJDIR)/%.o)
CC	:= m68k-amigaos-gcc
STRIP   := m68k-amigaos-strip
VLINK   := vlink
VASM    := vasmm68k_mot
NDK_PATH:= /opt/amiga-2021.05/m68k-amigaos/ndk-include
CFLAGS  := -DBUILD_DATE=\"$(DATE)\" -DBUILD_TIME=\"$(TIME)\"
CFLAGS  += -D_KERNEL -DPORT_AMIGA
#CFLAGS  += -DDEBUG            # Show basic debug
#CFLAGS  += -DDEBUG_SYNC       # Show Synchronous SCSI debug
#CFLAGS  += -DDEBUG_CMD        # Show handler commands received
# Per file debugging
#CFLAGS  += -DDEBUG_ATTACH     # Debug attach.c
#CFLAGS  += -DDEBUG_DEVICE     # Debug device.c
#CFLAGS  += -DDEBUG_CMDHANDLER # Debug commandhandler.c
#CFLAGS  += -DDEBUG_NCR53CXXX  # Debug ncr53cxxx.c
#CFLAGS  += -DEBUG_PORT        # Debug port.c
#CFLAGS  += -DEBUG_SCSIPI_BASE # Debug scsipi_base.c
#CFLAGS  += -DEBUG_SCSICONF    # Debug scsiconf.c
#CFLAGS  += -DEBUG_SD          # Debug sd.c
#CFLAGS  += -DEBUG_SIOP        # Debug siop.c
#CFLAGS2 += -DEBUG_MOUNTER     # Debug mounter.c # Makes more sense when integrated

CFLAGS  += -DENABLE_SEEK  # Not needed for modern drives (~500 bytes)
#CFLAGS  += -DNO_SERIAL_OUTPUT # Turn off serial debugging for the whole driver
#CFLAGS  += -mhard-float
CFLAGS  += -Wall -Wno-pointer-sign -fomit-frame-pointer
CFLAGS  += -Wno-strict-aliasing
CFLAGS2 := -Wall -Wno-pointer-sign -fomit-frame-pointer

LDFLAGS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -fomit-frame-pointer -nostartfiles -ldebug -nostdlib -lgcc -lc -lamiga
LDFLAGS2 = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -fomit-frame-pointer -mcrt=clib2 -lgcc -lc -lamiga

#CFLAGS  += -g
#LDFLAGS += -g

# Add link-time optimization (must also have optimization specified in LDFLAGS)
# -flto

# These don't work when compiled as a .device
#CFLAGS  += -fbaserel
#LDFLAGS += -fbaserel
CFLAGS  += -ramiga-dev
LDFLAGS += -ramiga-dev

CFLAGS  += -Os
LDFLAGS += -Os
#CFLAGS  += -O0
#LDFLAGS += -O0
#CFLAGS  += -fbbb=-
#LDFLAGS += -fbbb=-

QUIET   := @
QUIET   :=
SC_ASM	:= $(OBJDIR)/ncr53cxxx
SIOP_SCRIPT := $(OBJDIR)/siop_script.out

ifeq (, $(shell which $(CC) 2>/dev/null ))
$(error "No $(CC) in PATH: maybe do PATH=$$PATH:/opt/amiga/bin")
endif

all: $(PROG) $(PROG2) $(ROM)

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(OBJDIR)/%,$(filter-out $(OBJDIR)/%,$(basename $(1)).o)) $(filter $(OBJDIR)/%,$(basename $(1)).o): $(1)
endef
$(foreach SRCFILE,$(SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE))))
$(foreach SRCFILE,$(SRCS2),$(eval $(call DEPEND_SRC,$(SRCFILE))))

$(OBJDIR)/version.o: version.h $(filter-out $(OBJDIR)/version.o, $(OBJS))
$(OBJDIR)/siop.o: $(SIOP_SCRIPT)
$(OBJDIR)/siop.o:: CFLAGS += -I$(OBJDIR)
$(OBJDIR)/attach.o: device.h

# XXX: Need to generate real dependency files
$(OBJS): attach.h port.h scsi_message.h scsipiconf.h version.h port_bsd.h scsi_spc.h sd.h cmdhandler.h printf.h scsipi_base.h siopreg.h device.h scsi_all.h scsipi_debug.h siopvar.h scsi_disk.h scsipi_disk.h sys_queue.h

$(OBJS): Makefile port.h | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) -c $(filter %.c,$^) -o $@

$(OBJS2): Makefile | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS2) -c $(filter %.c,$^) -o $@

$(PROG): $(OBJS)
	@echo Building $@
	$(QUIET)$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(STRIP) $@

$(PROG2): $(OBJS2)
	@echo Building $@
	$(QUIET)$(CC) $(OBJS2) $(LDFLAGS2) -o $@

$(SIOP_SCRIPT): siop_script.ss $(SC_ASM)
	@echo Building $@
	$(SC_ASM) $(filter %.ss,$^) -p $@

$(SC_ASM): ncr53cxxx.c
	@echo Building $@
	$(QUIET)cc -o $@ $^

$(OBJDIR)/rom.o: rom.S reloc.S $(PROG)
	@echo Building $@
	$(QUIET)$(VASM) -m68020 -Fhunk -o $@ $< -I $(NDK_PATH)

$(OBJDIR)/reloc.o: reloc.S $(PROG)
	$(QUIET)$(VASM) -m68020 -Fhunk -o $@ $< -I $(NDK_PATH) -DUSERLAND=1

$(OBJDIR)/reloctest.o: reloctest.c
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS2) -c $^ -o $@

reloctest: $(OBJDIR)/reloctest.o $(OBJDIR)/reloc.o
	@echo Building $@
	$(QUIET)$(CC) $^ $(LDFLAGS2) -o $@

test: reloctest
	@echo Running relocation test
	vamos reloctest

a4091.rom: $(OBJDIR)/rom.bin
	@echo Building $@
	cp $< $@

$(OBJDIR)/rom.bin: $(OBJDIR)/rom.o rom.ld
	@echo Building $@
	$(QUIET)$(VLINK) -Trom.ld -brawbin1 -o $@ $<
	@test `wc -c <$<` -gt 32768 && echo "ROM FILE EXCEEDS 32K!" || echo "ROM fits in 32k"

$(OBJDIR):
	mkdir -p $@

clean:
	rm -f $(OBJS) $(OBJS2) $(OBJDIR)/*.map $(OBJDIR)/*.lst $(SIOP_SCRIPT) $(SC_ASM)
	rm -f $(OBJDIR)/rom.o $(OBJDIR)/rom.bin reloctest

distclean: clean
	rm -f $(PROG) $(PROG2) $(ROM)
	rm -r $(OBJDIR)
