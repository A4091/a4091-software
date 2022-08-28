#ifndef SCSIMSG_H
#define SCSIMSG_H 1

uint16_t sdcmd_read_blocks(struct IOStdReq * ioreq, uint8_t* data, uint32_t block, uint32_t len);

int safe_open(struct IOStdReq *ioreq, uint scsi_unit);
void safe_close(struct IOStdReq *ioreq);

#endif
