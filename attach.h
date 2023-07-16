#ifndef _ATTACH_H
#define _ATTACH_H

struct scsipi_periph;
struct sio_softc;
struct ExecBase;
struct Task;
struct Interrupt;
struct siop_softc;
struct MsgPort;
struct timerequest;
struct callout;
struct ConfigDev;

typedef struct {
    uint32_t              as_addr;
    struct ExecBase      *as_SysBase;
    int8_t                as_timer_running;
    uint8_t               as_irq_signal;
    uint32_t              as_irq_count;   // Total interrupts
    uint32_t              as_int_mask;
    uint32_t              as_timer_mask;
    struct Task          *as_svc_task;
    struct Interrupt     *as_isr;         // My interrupt server
    volatile uint8_t      as_exiting;
    struct siop_softc    *as_device_private;
    struct MsgPort       *as_timerport[2];
    struct timerequest   *as_timerio[2];
    struct callout      **as_callout_head;
    struct ConfigDev     *as_cd;
    uint32_t             romfile[2];
    /* battmem */
    uint8_t              cdrom_boot;
    uint8_t              ignore_last;
} a4091_save_t;

extern a4091_save_t *asave;

int attach(device_t self, uint scsi_target, struct scsipi_periph **periph,
           uint flags);
void detach(struct scsipi_periph *periph);
int periph_still_attached(void);
int init_chan(device_t self, UBYTE *boardnum);
void deinit_chan(device_t self);

#endif /* _ATTACH_H */

