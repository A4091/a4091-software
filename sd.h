#ifndef _MY_SD_H
#define _MY_SD_H

int diskstart(uint64_t blkno, uint b_flags, void *buf, uint buflen, void *ior);
void sd_complete(void *xsp, int rc);

#endif /* _MY_SD_H */
