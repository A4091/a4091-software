#ifndef _ATTACH_H
#define _ATTACH_H

int attach(device_t self, uint scsi_target, struct scsipi_periph *periph);
void detach(struct scsipi_periph *periph);

#endif /* _ATTACH_H */

