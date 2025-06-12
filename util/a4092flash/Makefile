PROJECT=a4092flash
CC=m68k-amigaos-gcc
CFLAGS=-lamiga -mcrt=nix13 -mcpu=68060 -Wall -Wno-pointer-sign -s -Os -fomit-frame-pointer
.PHONY:	clean all

ifneq ($(DEVICE_VERSION), )
CFLAGS+=-DDEVICE_VERSION=$(DEVICE_VERSION) -DDEVICE_REVISION=$(DEVICE_REVISION)
CFLAGS+=-DGIT_REF=$(GIT_REF) -DBUILD_DATE=$(BUILD_DATE)
endif

all:	$(PROJECT)

OBJ =   flash.o \
	config.o \
	main.o

SRCS = $(OBJ:%.o=%.c)


a4092flash: $(SRCS) *.h
	${CC} -o $@ $(CFLAGS) $(SRCS)

clean:
	-rm -rf $(PROJECT)
