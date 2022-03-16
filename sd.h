#ifndef _MY_SD_H
#define _MY_SD_H

int diskstart(struct scsipi_periph *periph, uint64_t blkno, uint b_flags,
              void *buf, uint buflen, void *ior);
int scsidirect(struct scsipi_periph *periph, void *cmd_p, void *ior);

void sd_complete(void *xsp, int rc);

#endif /* _MY_SD_H */
