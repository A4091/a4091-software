#ifndef _CMD_HANDLER_H
#define _CMD_HANDLER_H

int open_unit(uint scsi_target, void **io_Unit);
void close_unit(void *io_Unit);

int create_cmd_handler(uint scsi_target, void *io_Unit);
void amiga_sd_complete(void *ior, int8_t rc);

#endif /* _CMD_HANDLER_H */

