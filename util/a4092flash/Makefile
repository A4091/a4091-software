PROJECT=a4092flash
CC=m68k-amigaos-gcc
STRIP=m68k-amigaos-strip
HOSTCC=gcc
# Compiler flags (do not place libraries here)
CFLAGS=-mcrt=nix13 -mcpu=68060 -Wall -Wno-pointer-sign -s -Os -fomit-frame-pointer
# Libraries must come last on the link line
LDLIBS=-lamiga -lgcc
QUIET?=@
.PHONY:	clean all

GIT_REF_NAME = $(shell git branch --show-current)
GIT_REF := "$(GIT_REF_NAME)-$(shell git rev-parse --short HEAD)"
BUILD_DATE := $(shell date '+%-d.%-m.%Y')
DEVICE_VERSION  ?= 1
DEVICE_REVISION ?= 0

ifneq ($(DEVICE_VERSION), )
CFLAGS+=-DDEVICE_VERSION=$(DEVICE_VERSION) -DDEVICE_REVISION=$(DEVICE_REVISION)
CFLAGS+=-DGIT_REF=$(GIT_REF) -DBUILD_DATE=$(BUILD_DATE)
endif

# Flash type configuration - enable both by default, for the
# a4092flash utility the space consumed is not critical but
# in the driver we don't want to carry a parralel flash driver
# if the verilog has parallel flash disabled.
#
# Override with: make FLASH_PARALLEL=0 or make FLASH_SPI=0
FLASH_PARALLEL ?= 1
FLASH_SPI ?= 1

ifeq ($(FLASH_PARALLEL), 1)
CFLAGS += -DFLASH_PARALLEL=1
endif

ifeq ($(FLASH_SPI), 1)
CFLAGS += -DFLASH_SPI=1
endif

all:	$(PROJECT) spiutil

SRCS = flash.c \
       spi.c \
       config.c \
       main.c \
       nvram_flash.c \
       cpu_support.c


a4092flash: $(SRCS) *.h
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)
	$(QUIET)$(STRIP) -s $@

spiutil: spiutil.c cpu_support.c
	@echo Building $@
	$(QUIET)$(CC) $(CFLAGS) -o $@ spiutil.c cpu_support.c $(LDLIBS)

test:
	@echo Building NVRAM tests
	$(HOSTCC) -o nvram_test nvram_test.c
	@echo Running NVRAM tests
	./nvram_test

clean:
	-rm -rf $(PROJECT) nvram_test spiutil
