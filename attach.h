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

int attach(device_t self, uint scsi_target, struct scsipi_periph **periph);
void detach(struct scsipi_periph *periph);
int init_chan(device_t self, UBYTE *boardnum);
void deinit_chan(device_t self);

typedef struct {
    uint32_t              as_addr;
    struct ExecBase      *as_SysBase;
    uint32_t              as_irq_count;   // Total interrupts
    struct Task          *as_svc_task;
    struct Interrupt     *as_isr;         // My interrupt server
    uint8_t               as_irq_signal;
    volatile uint8_t      as_exiting;
    struct device         as_device_self;
    struct siop_softc    *as_device_private;
    struct MsgPort       *as_timerport[2];
    struct timerequest   *as_timerio[2];
    int                   as_timer_running;
    struct callout      **as_callout_head;
    struct ConfigDev     *as_cd;
    uint32_t             romfile[2];
} a4091_save_t;

#endif /* _ATTACH_H */

