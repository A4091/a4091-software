// SPDX-License-Identifier: GPL-2.0-only
/* This file is part of a4092flash
 * Copyright (C) 2023 Matthew Harlum <matt@harlum.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <stdbool.h>
#include <proto/exec.h>
#include <stdio.h>

#include "main.h"
#include "config.h"

/** configure
 *
 * @brief Parse the command arguments and return the config
 * @param argc Arg count
 * @param argv Argument variables
 * @returns Pointer to a Config struct or NULL on error
*/
struct Config* configure(int argc, char *argv[]) {

  bool error = false;

  struct Config *config;
  config = (struct Config *)AllocMem(sizeof(struct Config),MEMF_CLEAR);

  if (config == NULL) return NULL;

  config->eraseFlash       = false;
  config->rebootRequired   = false;
  config->assumeYes        = false;

  for (int i=1; i<argc; i++) {
    if (argv[i][0] == '-') {
      switch(argv[i][1]) {
        case 'W':
          if (i+1 < argc) {
            config->scsi_rom_filename = argv[i+1];
	    config->writeFlash = true;
            i++;
          }
          break;

        case 'R':
          if (i+1 < argc) {
            config->scsi_rom_filename = argv[i+1];
	    config->readFlash = true;
            i++;
          }
          break;

        case 'E':
          config->eraseFlash = true;
          break;

        case 'B':
          config->rebootRequired = true;
          break;

        case 'P':
          config->probeFlash = true;
          break;

        case 'Y':
          config->assumeYes = true;
          break;

      }
    }
  }

  if (config->readFlash == false && config->writeFlash == false &&
		 config->eraseFlash == false && config->probeFlash == false) {
      printf("You need to specify one of -E, -R, -W, -P.\n");
      error = true;
  }
  if (config->readFlash == true && config->writeFlash == true) {
      printf("a4092flash: -R, -W are mutually exclusive.\n");
      error = true;
  }

  if (error) {
    FreeMem(config,sizeof(struct Config));
    return (NULL);
  } else {
    return (config);
  }
}

/** usage
 * @brief Print the usage information
*/
void usage(void) {
    printf("\nUsage: a4092flash [-Y] { -R <a4092.rom> | -W <a4092.rom> | -E | -P }\n\n");
    printf("       -Y assume YES as answer to all questions\n");
    printf("       -R <a4092.rom> - Read A4092 ROM to file\n");
    printf("       -W <a4092.rom> - Flash A4092 ROM from file\n");
    printf("       -E Erase flash.\n");
    printf("       -P Probe flash.\n");
    printf("       -B Reboot.\n");
}
