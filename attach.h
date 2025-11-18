//
// Copyright 2022-2025 Chris Hooper & Stefan Reinauer
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//

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
    struct MsgPort       *as_timerport;
    struct timerequest   *as_timerio;
    struct callout      **as_callout_head;
    struct ConfigDev     *as_cd;
    /* battmem */
    uint8_t              cdrom_boot;
    uint8_t              ignore_last;
#ifdef ENABLE_QUICKINTS
    uint8_t              quick_int;
    /* quick interrupt support */
    ULONG                quick_vec_num;
#endif
} a4091_save_t;

extern a4091_save_t *asave;

int attach(device_t self, uint scsi_target, struct scsipi_periph **periph,
           uint flags);
void detach(struct scsipi_periph *periph);
int periph_still_attached(void);
int init_chan(device_t self, UBYTE *boardnum);
void deinit_chan(device_t self);

uint8_t get_dip_switches(void);
void decode_unit_number(ULONG unit_num, int *target, int *lun);
ULONG calculate_unit_number(int target, int lun);

#endif /* _ATTACH_H */

