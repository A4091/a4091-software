#ifndef SCSIMSG_H
#define SCSIMSG_H 1

#include <devices/trackdisk.h>
#include "scsipi_all.h"

typedef struct scsipi_inquiry_data scsi_inquiry_data_t;

int dev_scsi_inquiry(struct IOExtTD *tio, uint unit, scsi_inquiry_data_t *inq);
int dev_scsi_get_drivegeometry(struct IOExtTD *tio, struct DriveGeometry *geom);

#endif
