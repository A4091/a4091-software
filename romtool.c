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

#define ROMTOOL_VERSION "v0.2 (2023-11-22)"

#ifndef O_BINARY
#define O_BINARY 0
#endif

enum {
	DEVICE,
	FILESYSTEM1,
	FILESYSTEM2
};

struct file {
	char *addr;
	size_t len;
};

struct rom_inventory {
	uint32_t filesystem2_dostype, filesystem2_offset, filesystem2_len;
	uint32_t filesystem1_dostype, filesystem1_offset, filesystem1_len;
	uint32_t device_offset, device_len;
	uint32_t signature[2];
};

int is_compressed(char *image)
{
	uint32_t *file = (uint32_t *)image;
	if (ntohl(file[0]) == 0x524e4301)
		printf("rnc compressed (%x uncompressed)", ntohl(file[1]));
	if (ntohl(file[0]) == 0x5a583001)
		printf("zx0 compressed (%x uncompressed)", ntohl(file[1]));
	return (ntohl(file[0]) == 0x524e4301 || ntohl(file[0]) == 0x5a583001);
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

	if (ntohl(inv->filesystem1_len)) {
		printf("\n FileSystem 1: offset = 0x%06x length = 0x%06x ",
				ntohl(inv->filesystem1_offset), ntohl(inv->filesystem1_len));
		is_compressed(rom->addr + ntohl(inv->filesystem1_offset));
		printf("\n               DosType = 0x%08x", ntohl(inv->filesystem1_dostype));
	} else
		printf("\n FileSystem 1: <empty>");

	if (ntohl(inv->filesystem2_len)) {
		printf("\n FileSystem 2: offset = 0x%06x length = 0x%06x ",
			ntohl(inv->filesystem2_offset), ntohl(inv->filesystem2_len));
		is_compressed(rom->addr + ntohl(inv->filesystem2_offset));
		printf("\n               DosType = 0x%08x", ntohl(inv->filesystem2_dostype));
	} else
		printf("\n FileSystem 2: <empty>");
	printf("\n\n");

	int freebytes = rom->len - sizeof(struct rom_inventory) - ntohl(inv->device_offset);
	if (inv->filesystem1_len)
		freebytes -= ntohl(inv->filesystem1_len);
	if (inv->filesystem2_len)
		freebytes -= ntohl(inv->filesystem2_len);
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
	if (inv->filesystem1_len)
		freebytes -= ntohl(inv->filesystem1_len);
	if (inv->filesystem2_len)
		freebytes -= ntohl(inv->filesystem2_len);
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

int replace_file(struct file *rom, int num, struct file *file_to_insert, int dostype)
{
	struct rom_inventory *inv = (struct rom_inventory *)(rom->addr + rom->len -
			sizeof(struct rom_inventory));

	int new_space = 0;
	struct file local_filesystem1 = {NULL,0},
		    local_filesystem2 = {NULL,0},
		    device_content    = {NULL,0};

	// Flags to track which local struct file's .addr was allocated by file_backup
	int device_content_is_backup = 0;
	int filesystem1_is_backup = 0;
	int filesystem2_is_backup = 0;

	switch (num) {
	case DEVICE:
		new_space = ntohl(inv->device_offset) + file_to_insert->len + ntohl(inv->filesystem1_len) + ntohl(inv->filesystem2_len) + sizeof(struct rom_inventory);

		device_content = *file_to_insert;

		local_filesystem1.addr = file_backup(rom->addr+ntohl(inv->filesystem1_offset), ntohl(inv->filesystem1_len));
		local_filesystem1.len = ntohl(inv->filesystem1_len);
		if (local_filesystem1.addr) filesystem1_is_backup = 1;

		local_filesystem2.addr = file_backup(rom->addr+ntohl(inv->filesystem2_offset), ntohl(inv->filesystem2_len));
		local_filesystem2.len = ntohl(inv->filesystem2_len);
		if (local_filesystem2.addr) filesystem2_is_backup = 1;
		break;
	case FILESYSTEM1:
		new_space = ntohl(inv->device_offset) + ntohl(inv->device_len) + file_to_insert->len + ntohl(inv->filesystem2_len) + sizeof(struct rom_inventory);

		device_content.addr = file_backup(rom->addr+ntohl(inv->device_offset), ntohl(inv->device_len));
		device_content.len = ntohl(inv->device_len);
		if (device_content.addr) device_content_is_backup = 1;

		local_filesystem1 = *file_to_insert;

		local_filesystem2.addr = file_backup(rom->addr+ntohl(inv->filesystem2_offset), ntohl(inv->filesystem2_len));
		local_filesystem2.len = ntohl(inv->filesystem2_len);
		if (local_filesystem2.addr) filesystem2_is_backup = 1;

		inv->filesystem1_dostype = htonl(dostype);

		break;
	case FILESYSTEM2:
		new_space = ntohl(inv->device_offset) + ntohl(inv->device_len) + ntohl(inv->filesystem1_len) + file_to_insert->len + sizeof(struct rom_inventory);

		device_content.addr = file_backup(rom->addr+ntohl(inv->device_offset), ntohl(inv->device_len));
		device_content.len = ntohl(inv->device_len);
		if (device_content.addr) device_content_is_backup = 1;

		local_filesystem1.addr = file_backup(rom->addr+ntohl(inv->filesystem1_offset), ntohl(inv->filesystem1_len));
		local_filesystem1.len = ntohl(inv->filesystem1_len);
		if (local_filesystem1.addr) filesystem1_is_backup = 1;

		local_filesystem2 = *file_to_insert;

		inv->filesystem2_dostype = htonl(dostype);

		break;
	}

	if (new_space > rom->len) {
		printf("File can not fit into image (%zd bytes too big)\n",
				new_space - rom->len);
		// Free any allocated backups before returning
		if (device_content_is_backup && device_content.addr) free(device_content.addr);
		if (filesystem1_is_backup && local_filesystem1.addr) free(local_filesystem1.addr);
		if (filesystem2_is_backup && local_filesystem2.addr) free(local_filesystem2.addr);
		return -1;
	}

	/* Write device driver back */
	// First, clear the area where content will be placed (or the free space after the last item)
	memset(rom->addr + ntohl(inv->device_offset), 0xff, rom->len - ntohl(inv->device_offset) - sizeof(struct rom_inventory));
	if (device_content.addr && device_content.len > 0) { // Ensure there's content to copy
		memcpy(rom->addr + ntohl(inv->device_offset), device_content.addr, device_content.len);
	}
	inv->device_len = htonl(device_content.len); // This correctly reflects the new device length or existing if not replaced

	uint32_t current_offset = ntohl(inv->device_offset) + device_content.len;

	if (local_filesystem1.len > 0 && local_filesystem1.addr) { // Ensure there's content to copy
		inv->filesystem1_offset = htonl(current_offset);
		inv->filesystem1_len = htonl(local_filesystem1.len);
		memcpy(rom->addr + current_offset, local_filesystem1.addr, local_filesystem1.len);
		current_offset += local_filesystem1.len;
	} else {
		inv->filesystem1_offset = 0; // Or an offset indicating no file, e.g., current_offset if it's 0 length
		inv->filesystem1_len = 0;
	}

	if (local_filesystem2.len > 0 && local_filesystem2.addr) { // Ensure there's content to copy
		inv->filesystem2_offset = htonl(current_offset);
		inv->filesystem2_len = htonl(local_filesystem2.len);
		memcpy(rom->addr + current_offset, local_filesystem2.addr, local_filesystem2.len);
		// current_offset += local_filesystem2.len; // Not needed after this
	} else {
		inv->filesystem2_offset = 0;
		inv->filesystem2_len = 0;
	}

	// Free any allocated backups
	if (device_content_is_backup && device_content.addr) free(device_content.addr);
	if (filesystem1_is_backup && local_filesystem1.addr) free(local_filesystem1.addr);
	if (filesystem2_is_backup && local_filesystem2.addr) free(local_filesystem2.addr);

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
	       "   -T | --dostype <val>                  DosType (eg. 0x43443031)\n"
	       "   -s | --skip                           skip first filesystem slot\n"
	       "   -r | --resize [32|64}                 resize rom image to 32kB or 64kB\n"
	       "   -v | --version:                       print the version\n"
	       "   -h | --help:                          print this help\n\n");
}

int main(int argc, char *argv[])
{
	char *output_filename = NULL,
	     *device_filename = NULL,
	     *filesystem1_filename = NULL,
	     *filesystem2_filename = NULL;

	int fs_slot = 0, changed = 0;
	uint32_t newsize=0, filesystem1_dostype=0, filesystem2_dostype=0;
	struct file rom = {NULL,0},
		    device = {NULL,0},
		    filesystem1 = {NULL,0},
		    filesystem2 = {NULL,0};

	int opt, option_index = 0;
	static const struct option long_options[] = {
		{"output", 1, NULL, 'o'},
		{"device", 1, NULL, 'D'},
		{"filesystem", 1, NULL, 'F'},
		{"dostype", 1, NULL, 'T'},
		{"skip", 0, NULL, 's'},
		{"resize", 1, NULL, 'r'},
		{"version", 0, NULL, 'v'},
		{"help", 0, NULL, 'h'},
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argv, "o:D:F:T:sr:vh?",
					long_options, &option_index)) != EOF) {
		switch (opt) {
		case 'o':
			output_filename = strdup(optarg);
			break;
		case 'D':
			device_filename = strdup(optarg);
			break;
		case 'F':
			if (fs_slot == 0)
				filesystem1_filename = strdup(optarg);
			else if (fs_slot ==1)
				filesystem2_filename = strdup(optarg);
			else {
				printf("Only two filesystems supported\n");
				exit(1);
			}
			fs_slot++;
			break;
		case 'T':
			if (fs_slot == 0) {
				printf("Specify filesystem before DosType.\n");
				exit(1);
			} else if (fs_slot == 1) {
				filesystem1_dostype = strtol(optarg, NULL, 16);
			} else if (fs_slot == 2) {
				filesystem2_dostype = strtol(optarg, NULL, 16);
			}
			break;
		case 's':
			fs_slot++;
			if (fs_slot > 1) {
				printf("Only two filesystems supported\n");
				exit(1);
			}
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
		replace_file(&rom, DEVICE, &device, 0L);
		changed = 1;
	}

	if (filesystem1_filename) {
		filesystem1 = memorize_file(filesystem1_filename);
		replace_file(&rom, FILESYSTEM1, &filesystem1,
				filesystem1_dostype);
		changed = 1;
	}

	if (filesystem2_filename) {
		filesystem2 = memorize_file(filesystem2_filename);
		replace_file(&rom, FILESYSTEM2, &filesystem2,
				filesystem2_dostype);
		changed = 1;
	}

	inventory(output_filename, &rom);

	if (changed)
		write_file(output_filename, rom);

	if (filesystem1.addr)
		free(filesystem1.addr);
	if (filesystem2.addr)
		free(filesystem2.addr);
	if (device.addr)
		free(device.addr);
	free(rom.addr);

	return 0;
}
