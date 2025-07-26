//
// Copyright 2022-2025 Stefan Reinauer
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

#ifndef __BATTMEM_H
#define __BATTMEM_H

int Load_BattMem(void);
int Save_BattMem(void);

#define BATTMEM_A4091_CDROM_BOOT_ADDR 72
#define BATTMEM_A4091_CDROM_BOOT_LEN   1
#define BATTMEM_A4091_IGNORE_LAST_ADDR 73
#define BATTMEM_A4091_IGNORE_LAST_LEN   1

#endif
