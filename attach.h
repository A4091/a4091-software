#ifndef _ATTACH_H
#define _ATTACH_H

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
    struct siop_softc     as_device_private;
} a4091_save_t;

#endif /* _ATTACH_H */

