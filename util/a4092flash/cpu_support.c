#include <exec/execbase.h>
#include <proto/exec.h>
#include "cpu_support.h"

extern struct ExecBase *SysBase;

/* Global variable to store the original CACR state for 68030 */
ULONG g_OriginalCACR = 0;

/* Forward declarations for supervisor functions */
ULONG __stdargs Sup_DisableWA(void);
ULONG __stdargs Sup_RestoreWA(void);

void cpu_disable_write_allocation(void) {
    /* 1. Check if we are actually on a 68030 */
    if (!(SysBase->AttnFlags & AFF_68030)) {
        return;
    }

    /* 2. Define supervisor function inline (must use RTE not RTS) and call it */
    __asm__ __volatile__ (
        "       bra     1f                     \n"
        "       .globl  _Sup_DisableWA         \n"
        "_Sup_DisableWA:                       \n"
        "       movec   %%cacr,%%d0            \n"  // Read CACR
        "       move.l  %%d0,_g_OriginalCACR   \n"  // Save it (note underscore prefix)
        "       btst    #8,%%d0                \n"  // Test DCE bit (bit 8)
        "       beq.s   .skip_disable          \n"  // Skip if cache not enabled
        "       bclr    #13,%%d0               \n"  // Clear WA bit (bit 13)
        "       movec   %%d0,%%cacr            \n"  // Write back
        ".skip_disable:                        \n"
        "       moveq   #0,%%d0                \n"  // Return 0
        "       rte                            \n"  // Return from exception
        "1:                                    \n"
        :
        :
        : "d0", "cc", "memory"
    );
    Supervisor(Sup_DisableWA);
}

void cpu_restore_write_allocation(void) {
    /* 1. Check if we are on a 68030 */
    if (!(SysBase->AttnFlags & AFF_68030)) {
        return;
    }

    /* 2. Define supervisor function inline (must use RTE not RTS) and call it */
    __asm__ __volatile__ (
        "       bra     1f                     \n"
        "       .globl  _Sup_RestoreWA         \n"
        "_Sup_RestoreWA:                       \n"
        "       move.l  _g_OriginalCACR,%%d0   \n"  // Load saved CACR (note underscore prefix)
        "       movec   %%d0,%%cacr            \n"  // Restore it
        "       moveq   #0,%%d0                \n"  // Return 0
        "       rte                            \n"  // Return from exception
        "1:                                    \n"
        :
        :
        : "d0", "cc", "memory"
    );
    Supervisor(Sup_RestoreWA);
}
