#ifndef _MY_SD_H
#define _MY_SD_H

int sd_diskstart(void *periph, uint64_t blkno, uint b_flags,
                 void *buf, uint buflen, void *ior);
int sd_scsidirect(void *periph, void *cmd_p, void *ior);
int sd_getgeometry(void *periph, void *buf, void *ior);
uint32_t sd_blocksize(void *periph_p);

#endif /* _MY_SD_H */
