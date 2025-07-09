//
// Copyright 2022-2025 Stefan Reinauer & Chris Hooper
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

#ifndef __A4091_H
#define __A4091_H

#define A4091_OFFSET_AUTOCONFIG 0x00000000
#define A4091_OFFSET_ROM        0x00000000
#define A4091_OFFSET_REGISTERS  0x00800000
#define A4091_OFFSET_QUICKINT   0x00880003
#define A4091_OFFSET_SWITCHES   0x008c0003

#define A4000T_SCSI_BASE        0x00dd0000
#define A4000T_OFFSET_REGISTERS 0x00000040
#define A4000T_OFFSET_SWITCHES  0x00003000

#define A4000T770_SCSI_BASE        0x00dd0000
#define A4000T770_OFFSET_REGISTERS 0x00000000
#define A4000T770_OFFSET_SWITCHES  0x00003000

#define ZORRO_MFG_COMMODORE     0x0202
#define ZORRO_PROD_A4091        0x0054

#define A4091_INTPRI 30
#define A4091_IRQ    3

#if defined(DRIVER_A4091)
# define HW_OFFSET_REGISTERS     A4091_OFFSET_REGISTERS
# define HW_OFFSET_SWITCHES      A4091_OFFSET_SWITCHES
# define HW_CLOCK_FREQ           50     /* SCSI Clock = 50 MHz */
# define HW_IS_ZORRO3            1
#elif defined(DRIVER_A4000T)
# define HW_SCSI_BASE            A4000T_SCSI_BASE
# define HW_OFFSET_REGISTERS     A4000T_OFFSET_REGISTERS
# define HW_OFFSET_SWITCHES      A4000T_OFFSET_SWITCHES
# define HW_CLOCK_FREQ           50     /* SCSI Clock = 50 MHz */
# define HW_IS_ZORRO3            0
#elif defined(DRIVER_A4000T770)
# define HW_SCSI_BASE            A4000T770_SCSI_BASE
# define HW_OFFSET_REGISTERS     A4000T770_OFFSET_REGISTERS
# define HW_OFFSET_SWITCHES      A4000T770_OFFSET_SWITCHES
# define HW_CLOCK_FREQ           100    /* SCSI Clock = 100 MHz */
# define HW_IS_ZORRO3            0
#endif

#endif /* __A4091_H */
