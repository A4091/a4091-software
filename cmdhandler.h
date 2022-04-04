#ifndef _CMD_HANDLER_H
#define _CMD_HANDLER_H

int open_unit(uint scsi_target, void **io_Unit);
void close_unit(void *io_Unit);

int start_cmd_handler(uint scsi_target, void *io_Unit);
void stop_cmd_handler(void *io_Unit);
void cmd_complete(void *ior, int8_t rc);

#define CMD_TERM     0x7ff0  // Terminate command handler (end process)
#define CMD_STARTUP  0x7ff1
#define CMD_ATTACH   0x7ff2  // Attach (open) SCSI peripheral
#define CMD_DETACH   0x7ff3  // Detach (close) SCSI peripheral

#endif /* _CMD_HANDLER_H */

