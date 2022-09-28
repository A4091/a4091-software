#ifndef __AMIGAHW_H
#define __AMIGAHW_H

/* Amiga hardware registers */
#define REG_CIAAPRA      0xBFE001  // CIA-A Port A input bits [Read/Write]
#define REG_CIAAPRA_PA0  (1<<0)    // CIA-A OVL ROM/RAM Overlay Control
#define REG_CIAAPRA_PA1  (1<<1)    // CIA-A LED / Audio Filter (0 = bright)
#define REG_CIAAPRA_PA2  (1<<2)    // CIA-A CHNG Disk Change
#define REG_CIAAPRA_PA3  (1<<3)    // CIA-A WPRO Write Protect
#define REG_CIAAPRA_PA4  (1<<4)    // CIA-A TK0 Disk Track 0
#define REG_CIAAPRA_PA5  (1<<5)    // CIA-A RDY Disk Ready
#define REG_CIAAPRA_PA6  (1<<6)    // CIA-A Game Port 0 fire button (pin 6)
#define REG_CIAAPRA_PA7  (1<<7)    // CIA-A Game Port 1 fire button (pin 6)

#define REG_POTGOR       0xDFF016  // Paula proportional pin values [Read]
#define REG_POTGO        0xDFF034  // Paula proportional pin config [Write]
#define REG_POTGOR_DATLX (1<<8)    // Paula Pin 32 P0X Data
#define REG_POTGOR_OUTLX (1<<9)    // Paula Pin 32 P0X Output enable
#define REG_POTGOR_DATLY (1<<10)   // Paula Pin 33 P0Y Data
#define REG_POTGOR_OUTLY (1<<11)   // Paula Pin 33 P0Y Output enable
#define REG_POTGOR_DATRX (1<<12)   // Paula Pin 35 P1X Data
#define REG_POTGOR_OUTRX (1<<13)   // Paula Pin 35 P1X Output enable
#define REG_POTGOR_DATRY (1<<14)   // Paula Pin 36 P1Y Data
#define REG_POTGOR_OUTRY (1<<15)   // Paula Pin 36 P1Y Output enable

#endif
