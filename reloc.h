//
// Copyright 2022-2023 Stefan Reinauer
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//

#ifndef __RELOC_H
#define __RELOC_H 1

uint32_t relocate(ULONG offset asm("d0"), uint32_t program asm("a0"));
extern uint32_t rErrno;
extern uint32_t ReadHandle[2];

#endif
