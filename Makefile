NOW     := $(shell date)
DATE    := $(shell date -d '$(NOW)' '+%Y-%m-%d')
TIME    := $(shell date -d '$(NOW)' '+%H:%M:%S')

PROG	:= drv
OBJDIR  := objs
SRCS    := device.c version.c siop.c port.c attach.c cmdhandler.c printf.c
SRCS    += sd.c scsipi_base.c scsiconf.c
#SRCS    += main.c request.c
#SRCS    := device.c
OBJS    := $(SRCS:%.c=$(OBJDIR)/%.o)
CC	:= m68k-amigaos-gcc
CFLAGS  := -DBUILD_DATE=\"$(DATE)\" -DBUILD_TIME=\"$(TIME)\"
CFLAGS  += -D_KERNEL -DPORT_AMIGA
#CFLAGS  += -DDEBUG        # Show basic debug
#CFLAGS  += -DDEBUG_SYNC   # Show Synchronous SCSI debug
#CFLAGS  += -DDEBUG_CMD    # Show handler commands received
CFLAGS  += -DNO_SERIAL_OUTPUT
#CFLAGS  += -mhard-float
CFLAGS  += -Wall -Wno-pointer-sign -fomit-frame-pointer
CFLAGS  += -Wno-strict-aliasing

LDFLAGS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -fomit-frame-pointer -mcrt=clib2

#LDFLAGS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -fomit-frame-pointer -nostartfiles -mcrt=clib2 -ldebug -nostdlib -lgcc -lc -lamiga
LDFLAGS = -Xlinker -Map=$(OBJDIR)/$@.map -Wa,-a > $(OBJDIR)/$@.lst -fomit-frame-pointer -nostartfiles -ldebug -nostdlib -lgcc -lc -lamiga

#CFLAGS  += -g
#LDFLAGS += -g

# Add link-time optimization (must also have optimization specified in LDFLAGS)
# -flto

# These don't work when compiled as a .device
#CFLAGS  += -fbaserel
#LDFLAGS += -fbaserel
CFLAGS  += -ramiga-dev
LDFLAGS += -ramiga-dev

# % make -f Makefile.device debug
# m68k-amigaos-gcc -m68000 -Wall -Wextra -Wno-unused-parameter -fomit-frame-pointer -I. -g -O2 -DDEBUG -mcrt=clib2 -c -o build-debug/device.o device.c
# 68k-amigaos-gcc -m68000 -Wall -Wextra -Wno-unused-parameter -fomit-frame-pointer -I. -g -O2 -DDEBUG -mcrt=clib2 -o build-debug/a4091.device build-debug/device.o -nostdlib -nostartfiles -ldebug -Wl,-Map=build-debug/a4091.device.map
# 68k-amigaos-objdump -D build-debug/a4091.device > build-debug/a4091.device.s

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

all: $(PROG)

define DEPEND_SRC
# The following line creates a rule for an object file to depend on a
# given source file.
$(patsubst %,$(OBJDIR)/%,$(filter-out $(OBJDIR)/%,$(basename $(1)).o)) $(filter $(OBJDIR)/%,$(basename $(1)).o): $(1)
endef
$(foreach SRCFILE,$(SRCS),$(eval $(call DEPEND_SRC,$(SRCFILE))))

$(OBJDIR)/version.o: version.h $(filter-out $(OBJDIR)/version.o, $(OBJS))
$(OBJDIR)/siop.o: $(SIOP_SCRIPT)
$(OBJDIR)/siop.o:: CFLAGS += -I$(OBJDIR)
$(OBJDIR)/attach.o: device.h

# XXX: Need to generate real dependency files
$(OBJS): attach.h port.h scsi_message.h scsipiconf.h version.h port_bsd.h scsi_spc.h sd.h cmdhandler.h printf.h scsipi_base.h siopreg.h device.h scsi_all.h scsipi_debug.h siopvar.h dkvar.h scsi_disk.h scsipi_disk.h sys_queue.h

$(OBJS): Makefile port.h | $(OBJDIR)
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) -c $(filter %.c,$^) -o $@

$(PROG): $(OBJS)
	@echo Building $@
	$(QUIET)$(CC) $(OBJS) $(LDFLAGS) -o $@

$(SIOP_SCRIPT): siop_script.ss $(SC_ASM)
	@echo Building $@
	$(SC_ASM) $(filter %.ss,$^) -p $@

$(SC_ASM): ncr53cxxx.c
	@echo Building $@
	$(QUIET)cc -o $@ $^

$(OBJDIR):
	mkdir -p $@

clean:
	rm -f $(OBJS) $(OBJDIR)/*.map $(OBJDIR)/*.lst $(SIOP_SCRIPT) $(SC_ASM)
