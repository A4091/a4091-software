/* A4091 romtool
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

#define ROMTOOL_VERSION "v0.1 (2023-11-04)"

#ifndef O_BINARY
#define O_BINARY 0
#endif

enum {
	DEVICE,
	FILESYSTEM
};

struct file {
	char *addr;
	size_t len;
};

struct rom_inventory {
	uint32_t filesystem_offset, filesystem_len;
	uint32_t device_offset, device_len;
	uint32_t signature[2];
};

int is_compressed(char *image)
{
	uint32_t *file = (uint32_t *)image;
	if (ntohl(file[0]) == 0x524e4301)
		printf("compressed (%x uncompressed)", ntohl(file[1]));
	return (ntohl(file[0]) == 0x524e4301);
}

void inventory(char *filename, struct file *rom)
{
	struct rom_inventory *inv = (struct rom_inventory *)(rom->addr + rom->len -
			sizeof(struct rom_inventory));

	if (ntohl(inv->signature[0]) != 0xFFFF5352 ||
		ntohl(inv->signature[1]) != 0x2F434448) {
		printf("%s: %dkB A4091 ROM image. Signature: %08x%08x (INVALID)\n\n",
			filename, (rom->len==32768) ? 32:64,
			ntohl(inv->signature[0]), ntohl(inv->signature[1]));
		return;
	}
	printf("%s: %dkB A4091 ROM image. Signature: OK\n\n",
			filename, (rom->len==32768) ? 32:64);

	printf(" ROM header:   offset = 0x000000 length = 0x%06x\n",
			ntohl(inv->device_offset));
	printf(" a4091.device: offset = 0x%06x length = 0x%06x ",
			ntohl(inv->device_offset), ntohl(inv->device_len));
	is_compressed(rom->addr + ntohl(inv->device_offset));

	printf("\n CDFileSystem: offset = 0x%06x length = 0x%06x ",
			ntohl(inv->filesystem_offset), ntohl(inv->filesystem_len));
	is_compressed(rom->addr + ntohl(inv->filesystem_offset));
	printf("\n\n");

	int freebytes = rom->len - sizeof(struct rom_inventory) - ntohl(inv->device_offset);
	if (inv->filesystem_len)
		freebytes -= ntohl(inv->filesystem_len);
	if (inv->device_len)
		freebytes -= ntohl(inv->device_len);

	printf(" %d bytes free (%2.2f%%)\n\n", freebytes, (float)freebytes/(float)rom->len * 100 );
}

char *file_backup(char *addr, size_t len)
{
	char *backup;
	if (len == 0)
		return NULL;
	backup = malloc(len);
	if (!backup) {
		printf("Out of memory.\n");
		exit(EXIT_FAILURE);
	}
	memcpy(backup, addr, len);
	return backup;
}

int resize(struct file *rom, int newsize)
{
	if ((newsize * 1024) == rom->len) {
		printf("Skip resize, ROM is already %dkb\n", newsize);
		return 0;
	}

	struct rom_inventory *inv = (struct rom_inventory *)(rom->addr + rom->len -
			sizeof(struct rom_inventory));
	struct rom_inventory backup = *inv;

	int freebytes = rom->len - sizeof(struct rom_inventory) - ntohl(inv->device_offset);
	if (inv->filesystem_len)
		freebytes -= ntohl(inv->filesystem_len);
	if (inv->device_len)
		freebytes -= ntohl(inv->device_len);

	if (newsize == 32 && freebytes < 32768) {
			printf("Not enough free space to resize. Missing %d bytes.\n",
					32768 - freebytes);
			return -1;
	}

	char *new_addr = malloc(newsize * 1024);
	if (!new_addr) {
		printf("Out of memory.\n");
		exit(EXIT_FAILURE);
	}

	if (newsize == 64)
		memset(new_addr + 32*1024, 0xff, 32*1024);

	memcpy(new_addr, rom->addr, 32 * 1024);
	free(rom->addr);
	rom->addr = new_addr;
	rom->len = newsize * 1024;

	inv = (struct rom_inventory *)(rom->addr + rom->len -
		sizeof(struct rom_inventory));
	*inv=backup;

	return 0;
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

int write_file(char *filename, struct file rom)
{
	int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC | O_BINARY,
			S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
	if (fd == -1) {
		fprintf(stderr, "%s: ",filename);
		perror("Could not open file");
		exit(EXIT_FAILURE);
	}

	if (write(fd, rom.addr, rom.len) != rom.len) {
		perror("Could not write file");
		exit(EXIT_FAILURE);
	}

	close(fd);

	return 0;
}

int replace_file(struct file *rom, int num, struct file *file)
{
	struct rom_inventory *inv = (struct rom_inventory *)(rom->addr + rom->len -
			sizeof(struct rom_inventory));

	int new_space = 0;
	struct file filesystem = {NULL,0},
		    device = {NULL,0};

	if (num == DEVICE) {
		new_space = ntohl(inv->device_offset) + file->len + ntohl(inv->filesystem_len) + sizeof(struct rom_inventory);
		device = *file;
		filesystem.addr = file_backup(rom->addr+ntohl(inv->filesystem_offset), ntohl(inv->filesystem_len));
		filesystem.len = ntohl(inv->filesystem_len);
	} else {
		new_space = ntohl(inv->device_offset) + ntohl(inv->device_len) + file->len + sizeof(struct rom_inventory);
		device.addr = file_backup(rom->addr+ntohl(inv->device_offset), ntohl(inv->device_len));
		device.len = ntohl(inv->device_len);
		filesystem = *file;
	}
	if (new_space > rom->len) {
		printf("File can not fit into image (%zd bytes too big)\n",
				new_space - rom->len);
		return -1;
	}

	inv->device_len = 0;
	inv->filesystem_offset = 0;
	inv->filesystem_len = 0;
	memset(rom->addr + ntohl(inv->device_offset), 0xff, rom->len - ntohl(inv->device_offset) - sizeof(struct rom_inventory));
	memcpy(rom->addr + ntohl(inv->device_offset), device.addr, device.len);
	inv->device_len = htonl(device.len);
	if (filesystem.len) {
		inv->filesystem_offset = htonl(ntohl(inv->device_offset)+ntohl(inv->device_len));
		inv->filesystem_len =htonl(filesystem.len);
		memcpy(rom->addr+ntohl(inv->filesystem_offset), filesystem.addr, filesystem.len);
	}
	return 0;
}

static void print_version(void)
{
	printf("romtool %s -- ", ROMTOOL_VERSION);
	printf("Copyright (C) 2023 Stefan Reinauer.\n\n");
}

static void print_usage(const char *name)
{
	printf("Usage: %s [-vh?] <filename>\n", name);
	printf("\n"
	       "   -o | --output <filename>              output filename\n"
	       "   -D | --device <filename>              path to a4091.device\n"
	       "   -F | --filesystem <filename>          path to CDFileSystem\n"
	       "   -r | --resize [32|64}                 resize rom image to 32kB or 64kB\n"
	       "   -v | --version:                       print the version\n"
	       "   -h | --help:                          print this help\n\n");
}

int main(int argc, char *argv[])
{
	char *output_filename = NULL,
	     *device_filename = NULL,
	     *filesystem_filename = NULL;

	int changed = 0;
	uint32_t newsize=0;
	struct file rom = {NULL,0},
		    device = {NULL,0},
		    filesystem = {NULL,0};

	int opt, option_index = 0;
	static const struct option long_options[] = {
		{"output", 1, NULL, 'o'},
		{"device", 1, NULL, 'D'},
		{"filesystem", 1, NULL, 'F'},
		{"resize", 1, NULL, 'r'},
		{"version", 0, NULL, 'v'},
		{"help", 0, NULL, 'h'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "o:D:F:r:vh?",
					long_options, &option_index)) != EOF) {
		switch (opt) {
		case 'o':
			output_filename = strdup(optarg);
			break;
		case 'D':
			device_filename = strdup(optarg);
			break;
		case 'F':
			filesystem_filename = strdup(optarg);
			break;
		case 'r':
			newsize = strtoul(optarg, NULL, 0);
			if (newsize != 32 && newsize != 64) {
				printf("Option --resize only supports 32 and 64.\n");
				exit(EXIT_FAILURE);
			}
			break;
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
	if (!output_filename)
		output_filename = filename;

	rom = memorize_file(filename);

	//int size = rom.len;
	//char *image = rom.addr;

	if (rom.len != (32*1024) && rom.len != (64*1024)) {
		printf("A4091 ROM file needs to be 32k or 64k in size\n");
		exit(EXIT_FAILURE);
	}

	if (newsize) {
		if (resize(&rom, newsize))
			exit(EXIT_FAILURE);
		if (newsize * 1024 != rom.len)
			changed = 1;
	}

	// TODO implement file removal

	if (device_filename) {
		device = memorize_file(device_filename);
		replace_file(&rom, DEVICE, &device);
		changed = 1;
	}

	if (filesystem_filename) {
		filesystem = memorize_file(filesystem_filename);
		replace_file(&rom, FILESYSTEM, &filesystem);
		changed = 1;
	}

	inventory(output_filename, &rom);

	if (changed)
		write_file(output_filename, rom);

	if (filesystem.addr)
		free(filesystem.addr);
	if (device.addr)
		free(device.addr);
	free(rom.addr);

	return 0;
}
