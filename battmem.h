#ifndef __BATTMEM_H
#define __BATTMEM_H

int Load_BattMem(void);
int Save_BattMem(void);

#define BATTMEM_A4091_CDROM_BOOT_ADDR 72
#define BATTMEM_A4091_CDROM_BOOT_LEN   1
#define BATTMEM_A4091_IGNORE_LAST_ADDR 73
#define BATTMEM_A4091_IGNORE_LAST_LEN   1

#endif
