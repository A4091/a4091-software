/* A4091 resident analyzer
 *
 * Copyright 2023 Stefan Reinauer
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS”
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#define RESIDENT_VERSION "v0.1 (2023-11-10)"

#ifndef O_BINARY
#define O_BINARY 0
#endif

struct file {
	char *addr;
	size_t len;
};

struct resident {
    uint16_t rt_MatchWord;
    uint32_t rt_MatchTag;
    uint32_t rt_EndSkip;
    uint8_t  rt_Flags;
    uint8_t  rt_Version;
    uint8_t  rt_Type;
    int8_t   rt_Pri;
    uint32_t rt_Name;
    uint32_t rt_IdString;
    uint32_t rt_Init;
} __attribute__((packed));

void dump_resident(char *addr, int i)
{
	struct resident *res = (struct resident *)(addr + i);
	// int offset = 32;
	int offset = i - ntohl(res->rt_MatchTag);

	printf("Resident structure found @ 0x%06x\n", i);
	printf("  00 rt_MatchWord = %04x\n", ntohs(res->rt_MatchWord));
	printf("  02 rt_MatchTag  = %08x (%s)\n",
			ntohl(res->rt_MatchTag),
			((ntohl(res->rt_MatchTag) + offset) == i) ? "ok" : "not ok");
	printf("  06 rt_EndSkip   = %08x (r+0x%x)\n",
			ntohl(res->rt_EndSkip),
			ntohl(res->rt_EndSkip) + offset - i);
	printf("  0a rt_Flags     = %02x\n", res->rt_Flags);
	printf("  0b rt_Version   = %02x\n", res->rt_Type);
	printf("  0c rt_Pri       = %d\n", res->rt_Pri);
	printf("  0d rt_Name      = %08x : %s\n",
			ntohl(res->rt_Name),
			addr + (ntohl(res->rt_Name) + offset));
	printf("  12 rt_IdString  = %08x : %s",
			ntohl(res->rt_IdString),
			addr + (ntohl(res->rt_IdString) + offset));
	printf("  16 rt_Init      = %08x\n", ntohl(res->rt_Init));
	printf("  1a\n");
}

void find_resident(struct file resmod)
{
	int i;
	for (i=0; i<resmod.len; i+=2) {
		if (ntohs(*(unsigned short *)(&resmod.addr[i])) != 0x4afc)
			continue;
		dump_resident(resmod.addr, i);
	}
}

struct file memorize_file(char *filename)
{
	struct file ret;

	int fd = open(filename, O_RDONLY | O_BINARY);
	if (fd == -1) {
		perror("Could not open file");
		exit(EXIT_FAILURE);
	}
	struct stat buf;
	if (fstat(fd, &buf) == -1) {
		perror("Could not stat file");
		exit(EXIT_FAILURE);
	}
	int size = buf.st_size;
	int aligned_size = (size + 15) & 0xfffffff0;

	char *image = malloc(aligned_size);
	if (!image) {
		printf("Out of memory.\n");
		exit(EXIT_FAILURE);
	}
	memset(image, 0, aligned_size);

	if (read(fd, image, size) != size) {
		perror("Could not read file");
		free(image);
		exit(EXIT_FAILURE);
	}

	close(fd);

	ret.addr = image;
	ret.len = aligned_size;

	return ret;
}

static void print_version(void)
{
	printf("resident %s -- ", RESIDENT_VERSION);
	printf("Copyright (C) 2023 Stefan Reinauer.\n\n");
}

static void print_usage(const char *name)
{
	printf("Usage: %s [-vh?] <filename>\n", name);
	printf("\n"
	       "   -v | --version:                       print the version\n"
	       "   -h | --help:                          print this help\n\n");
}

int main(int argc, char *argv[])
{
	struct file resmod = {NULL,0};

	int opt, option_index = 0;
	static const struct option long_options[] = {
		{"version", 0, NULL, 'v'},
		{"help", 0, NULL, 'h'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "vh?",
					long_options, &option_index)) != EOF) {
		switch (opt) {
		case 'v':
			print_version();
			exit(EXIT_SUCCESS);
			break;
		case 'h':
		case '?':
		default:
			print_usage(argv[0]);
			exit(EXIT_SUCCESS);
			break;

		}
	}

	if (optind + 1 != argc) {
		fprintf(stderr, "You need to specify a file.\n\n");
		fprintf(stderr, "run '%s -h' for usage\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	char *filename = argv[optind];

	resmod = memorize_file(filename);

	find_resident(resmod);

	free(resmod.addr);

	return 0;
}
