#ifndef _MY_SD_H
#define _MY_SD_H

int sd_readwrite(void *periph, uint64_t blkno, uint b_flags,
                 void *buf, uint buflen, void *ior);
int sd_seek(void *periph_p, uint64_t blkno, void *ior);
int sd_scsidirect(void *periph, void *cmd_p, void *ior);
int sd_getgeometry(void *periph, void *buf, void *ior);
int sd_get_protstatus(void *periph_p, ULONG *status);
int sd_startstop(void *periph_p, void *ior, int start, int load_eject,
                 int immed);
int sd_testunitready(void *periph_p, void *ior);

uint32_t sd_blocksize(void *periph_p);

#endif /* _MY_SD_H */
