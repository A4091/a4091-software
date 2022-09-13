/*
 * Test for reloc.S function
 */

#include <stdint.h>
#include <stdio.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <clib/exec_protos.h>
extern uint32_t relocate(ULONG offset asm("d0"), uint8_t *program asm("a0"));
extern uint32_t Hunks[];
extern uint32_t HunksLen[];
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
	int i;
	uint32_t ret;

	printf("Calling relocate()\n");
	ret=relocate(0, device);
	printf("... relocate returned %x\n", ret);

	for (i=0; i<4; i++) {
		if (Hunks[i] == 0)
			continue;
		printf("Hunk %d: %08x %8d\n", i, Hunks[i], HunksLen[i]);
		hexdump((unsigned char *)Hunks[i], HunksLen[i]);
		FreeMem((void *)Hunks[i], HunksLen[i]);
	}

	return(0);
}
