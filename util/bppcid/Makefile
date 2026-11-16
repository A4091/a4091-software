CC = m68k-amigaos-gcc
HOSTCC ?= cc
CFLAGS = -mcrt=clib2 -mcpu=68060 -Os -fno-builtin -Wall -Wextra -Werror

.PHONY: all disk test clean
all: bppcid

bppcid: main.c flash.c mapping.S flash.h
	$(CC) $(CFLAGS) -o $@ main.c flash.c mapping.S -lamiga
	m68k-amigaos-strip $@

# Keep this separate from the full SCSI utilities disk.
disk: bppcid
	$(MAKE) -C ../.. ncr7xx
	python3 build-backup-disk.py $(if $(ADF),"$(ADF)")

test: test-flash
	./test-flash

test-flash: test_flash.c flash.c flash.h
	$(HOSTCC) -std=c99 -O2 -Wall -Wextra -Werror -o $@ test_flash.c flash.c

clean:
	rm -f bppcid test-flash
