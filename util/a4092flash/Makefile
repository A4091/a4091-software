PROJECT=a4092flash
CC=m68k-amigaos-gcc
HOSTCC=gcc
CFLAGS=-lamiga -mcrt=nix13 -mcpu=68060 -Wall -Wno-pointer-sign -s -Os -fomit-frame-pointer
.PHONY:	clean all

GIT_REF_NAME = $(shell git branch --show-current)
GIT_REF := "$(GIT_REF_NAME)-$(shell git rev-parse --short HEAD)"
BUILD_DATE := $(shell date '+%-d.%-m.%Y')
DEVICE_VERSION  = 1
DEVICE_REVISION = 0

ifneq ($(DEVICE_VERSION), )
CFLAGS+=-DDEVICE_VERSION=$(DEVICE_VERSION) -DDEVICE_REVISION=$(DEVICE_REVISION)
CFLAGS+=-DGIT_REF=$(GIT_REF) -DBUILD_DATE=$(BUILD_DATE)
endif

all:	$(PROJECT)

OBJ =   flash.o \
	config.o \
	main.o \
	nvram_flash.o

SRCS = $(OBJ:%.o=%.c)


a4092flash: $(SRCS) *.h
	$(CC) -o $@ $(CFLAGS) $(SRCS)

test:
	$(HOSTCC) -o nvram_test nvram_test.c
	./nvram_test

clean:
	-rm -rf $(PROJECT) nvram_test
