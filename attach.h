#ifndef _ATTACH_H
#define _ATTACH_H

int attach(uint which, uint scsi_target);
void detach(void);
void irq_poll(uint got_int);
int create_cmd_handler(uint scsi_target);
void amiga_sd_complete(void *ior, int8_t rc);

#endif /* _ATTACH_H */

