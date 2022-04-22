#ifndef _CMD_HANDLER_H
#define _CMD_HANDLER_H

int open_unit(uint scsi_target, void **io_Unit);
void close_unit(void *io_Unit);

int start_cmd_handler(uint scsi_target);
void stop_cmd_handler(void);
void cmd_complete(void *ior, int8_t rc);

/* Trackdisk-64 enhanced commands */
#define TD_READ64    24      // Read at 64-bit offset
#define TD_WRITE64   25      // Write at 64-bit offset
#define TD_SEEK64    26      // Seek to 64-bit offset
#define TD_FORMAT64  27      // Format (write) at 64-bit offset

/* Internal commands */
#define CMD_TERM     0x2ef0  // Terminate command handler (end process)
#define CMD_STARTUP  0x2ef1  // Command handler startup message
#define CMD_ATTACH   0x2ff2  // Attach (open) SCSI peripheral
#define CMD_DETACH   0x2ef3  // Detach (close) SCSI peripheral

#endif /* _CMD_HANDLER_H */

