#if 0
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <exec/execbase.h>
#include <exec/types.h>
#include <exec/exec.h>
#include <proto/exec.h>
#include <sys/cdefs.h>
#endif

//#define DEBUG
// Global to cache BadQuickInt value
static __used ULONG BadQuickInt = 0;
static __used BOOL BadQuickIntFound = FALSE;

ULONG __stdargs GetQuickVec();
ULONG __stdargs ReleaseVec();
#undef ObtainQuickVector

#ifdef DEBUG
#define dprintf(x...) printf(x)
#else
#define dprintf(x...)
#endif

ULONG __stdargs ObtainQuickVector(APTR vector)
{
    ULONG vectorNum;
    asm volatile("move.l %0,d2" : : "g" (vector) : "d2");
    vectorNum = Supervisor(GetQuickVec);

    // Pass vector via register a0 and call supervisor
    asm volatile(
        "       bra 1f                         \n"
        "       .globl _GetQuickVec            \n"
        "_GetQuickVec:                         \n"
        "       move.l  d2,a0                  \n"
        "       movem.l d1-d5/a1-a3,-(sp)      \n"
        "       sub.l   a1,a1                  \n" // Clear a1 (assume VBR=0)
        "       or.w    #0x0700,sr             \n"
        "       btst    #0,(296+1)(a6)         \n" // AFB_68010 in AttnFlags+1
        "       beq.s   .NoVBR                 \n"
#ifdef STANDALONE
        ".chip 68010                           \n"
#endif
        "       movec vbr,a1                   \n"
#ifdef STANDALONE
        ".chip 68000                           \n"
#endif
        ".NoVBR:                               \n"
        // Find most common vector value between 68-255
        "       lea     0x110(a1),a2           \n" // Start at vector 68
        "       moveq   #0,d2                  \n" // Max count
        "       moveq   #0,d3                  \n" // BadQuickInt value

        // Outer loop - for each unique value
        "       move.l  #(255-68-1),d4         \n" // Counter for vectors 68-255
        ".FindLoop1:                           \n"
        "       move.l  (a2)+,d0               \n" // Get vector value
        "       moveq   #1,d1                  \n" // Count = 1

        // Inner loop - count occurrences
        "       move.l  a2,a3                  \n" // Start from next vector
        "       move.l  d4,d5                  \n" // Remaining vectors
        "       beq.s   .CheckMax              \n"
        "       subq.l  #1,d5                  \n"
        "       beq.s   .CheckMax              \n"

        ".FindLoop2:                           \n"
        "       cmp.l   (a3)+,d0               \n"

        "       bne.s   .Next2                 \n"
        "       addq.l  #1,d1                  \n" // Increment count
        ".Next2:                               \n"

        "       subq.l  #1,d5                  \n"
        "       bne.s   .FindLoop2             \n"

        ".CheckMax:                            \n"
        "       cmp.l   d2,d1                  \n" // Compare with max count
        "       ble.s   .Next1                 \n"
        "       move.l  d1,d2                  \n" // New max count
        "       move.l  d0,d3                  \n" // Save value as BadQuickInt
        ".Next1:                               \n"
        "       dbf     d4,.FindLoop1          \n"

        // Store BadQuickInt
        "       move.l  d3,_BadQuickInt        \n"
        "       move.w  #-1,_BadQuickIntFound  \n"

        ".SkipFind:                            \n"
        "       lea     0x400(a1),a2           \n" // Point at end of user vectors
        "       move.l  #255,d0                \n" // Last vector number
        "       move.l  _BadQuickInt,d1        \n" // Get BadQuickInt value

        ".SearchLoop:                          \n"
        "       cmp.l   -(a2),d1               \n" // Check if empty
        "       dbeq    d0,.SearchLoop         \n"
        "       bne.s   .NoVector              \n"

        "       cmp.w   #68,d0                 \n" // Check if >= 68
        "       bcs.s   .NoVector              \n"

        "       move.l  a0,(a2)                \n" // Store the vector
        "       movem.l (sp)+,d1-d5/a1-a3      \n"
        "       rte                            \n" // Return with vector number in d0

        ".NoVector:                            \n"
        "       moveq   #0,d0                  \n" // Return failure
        "       movem.l (sp)+,d1-d5/a1-a3      \n"
        "       rte                            \n"
        "1:                                    \n"
        : // No outputs
        :
        : "cc", "memory"
    );
    if (BadQuickIntFound) {
        dprintf("BadQuickInt = 0x%08"PRIx32"\n", BadQuickInt);
    } else {
        dprintf("BadQuickInt not found.\n");
    }
    return vectorNum;
}

// Function to release a quick vector
void ReleaseQuickVector(ULONG vectorNum)
{
    __unused APTR oldVec;

    // Validate vector number
    if (vectorNum < 68 || vectorNum > 255) {
        dprintf("Invalid QuickInt vector number.\n");
        return;  // Invalid vector number
    }

    // Must have found BadQuickInt before we can release
    if (!BadQuickIntFound) {
        dprintf("BadQuickInt was not found. Can't restore.\n");
        BadQuickInt = (ULONG)NULL;
        // Continue but restore with NULL
    }

    oldVec = (APTR)Supervisor(ReleaseVec);
    dprintf(" old vector = 0x%08"PRIx32"\n", (ULONG)oldVec);

    // Call supervisor code to restore the vector
    asm volatile(
        "       bra.s  1f                      \n"
        "       .globl _ReleaseVec             \n"
        "_ReleaseVec:                          \n"
        "       move.l  a1,-(sp)               \n"
        "       move.l  %0,d0                  \n" // Put vector number in d0
        "       or.w    #0x0700,sr             \n" // Disable interrupts
        // Get VBR
        "       sub.l   a1,a1                  \n" // Clear a1 (assume VBR=0)
        "       btst    #0,(296+1)(a6)         \n" // AFB_68010 in AttnFlags+1
        "       beq.s   .NoVBR2                \n"
#ifdef STANDALONE
        ".chip 68010                           \n"
#endif
        "       movec   vbr,a1                 \n"
#ifdef STANDALONE
        ".chip 68000                           \n"
#endif
        ".NoVBR2:                              \n"

        // Calculate vector address
        "       lsl.l   #2,d0                  \n" // Vector number * 4
        "       add.l   d0,a1                  \n" // Point to specific vector

        // Read Original QuickInt handler (for debugging)
        "       move.l (a1),d0                 \n"
        // Restore BadQuickInt
        "       move.l  _BadQuickInt,(a1)      \n"
        "       move.l  (sp)+,a1               \n"
        "       rte                            \n" // Return from exception
        "1:                                    \n"
        : // No outputs
        : "g" (vectorNum)
        : "cc", "memory"
    );
}
