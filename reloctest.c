//
// Copyright 2022-2023 Stefan Reinauer
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//

/*
 * Test for reloc.S function
 */

#include <stdint.h>
#include <stdio.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <clib/exec_protos.h>
uint32_t relocate(ULONG offset asm("d0"), uint8_t *program asm("a0"));
extern uint32_t rErrno;
extern uint8_t  device[];

static void hexdump(unsigned char *data, size_t size)
{
	char ascii[17];
	size_t i, j;
	ascii[16] = '\0';
	for (i = 0; i < size; ++i) {
		if (i % 16 == 0)
		printf("%6p: ", data+i);
		printf("%02X ", data[i]);
		if (data[i] >= ' ' && data[i] <= '~') {
			ascii[i % 16] = data[i];
		} else {
			ascii[i % 16] = '.';
		}
		if ((i+1) % 8 == 0 || i+1 == size) {
			printf(" ");
			if ((i+1) % 16 == 0) {
				printf("|  %s\n", ascii);
			} else if (i+1 == size) {
				ascii[(i+1) % 16] = '\0';
				if ((i+1) % 16 <= 8) {
					printf(" ");
				}
				for (j = (i+1) % 16; j < 16; ++j) {
					printf("   ");
				}
				printf("|  %s\n", ascii);
			}
		}
	}
}

int main(int argc, char *argv[])
{
	int i=0;
	uint32_t ret;
	uint32_t *seg;

	printf("Calling relocate()\n");
	ret=relocate(0, device);
	printf("... relocate returned %x (rErrno=0x%x)\n", ret, rErrno);

	seg=(uint32_t *)ret;

	do {
		uint32_t len=*(seg-1);
		uint32_t *hunk=(seg+1);
		printf("Hunk %d: %p  %08d\n", i, hunk,(len-2)<<2);
		hexdump((unsigned char *)hunk,(len-2)<<2);
		unsigned char *fptr=(unsigned char *)(seg-1);
		seg = BADDR(*seg);
		FreeMem(fptr, len<<2);
		i++;
	} while (seg);

	return 0;
}
