#ifndef __RELOC_H
#define __RELOC_H 1

uint32_t relocate(ULONG offset asm("d0"), uint32_t program asm("a0"));
extern uint32_t rErrno;
extern uint32_t ReadHandle[2];

#endif
