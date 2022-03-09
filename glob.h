#ifndef _GLOB_H
#define _GLOB_H

typedef struct {
    uint32_t           as_addr;
    struct DosLibrary *as_DOSBase;
    struct ExecBase   *as_SysBase;
    uint32_t           as_irq_count;   // Total interrupts
    struct Task       *as_svc_task;
    struct Interrupt  *as_isr;         // My interrupt server
    void              *as_device_private;
    uint8_t            as_irq_signal;
    volatile uint8_t   as_exiting;
} a4091_save_t;

extern a4091_save_t a4091_save;

#endif /* _GLOB_H */
