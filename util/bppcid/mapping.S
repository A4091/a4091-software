/* SPDX-License-Identifier: GPL-2.0-only */
/* 68060 only. Caller is supervisor, interrupts disabled, caches flushed.
 * PLPA checks translation/protection without accessing the target address.
 * Test every possible 4 KiB page, for both user and supervisor data accesses.
 * Use a private vector table so a protected original VBR page is untouched.
 */
        .text
        .even
        .globl _bppc_mapping_ok
_bppc_mapping_ok:
        movem.l %d2-%d5/%a2-%a3,-(%sp)
        movec   %vbr,%a2
        movec   %dfc,%d4
        lea     .Lvectors,%a3
        move.l  %a2,%a0
        move.l  %a3,%a1
        move.w  #255,%d0
.Lcopy:
        move.l  (%a0)+,(%a1)+
        dbra    %d0,.Lcopy
        lea     .Lfault(%pc),%a0
        move.l  %a0,8(%a3)
        move.l  %a0,16(%a3)
        movec   %a3,%vbr
        moveq   #1,%d5
.Lmode:
        movec   %d5,%dfc
        move.l  #0x00f00000,%d1
.Lpage:
        move.l  %d1,%a0
        plpar   (%a0)
        cmpa.l  %d1,%a0
        bne.s   .Lbad
        plpaw   (%a0)
        cmpa.l  %d1,%a0
        bne.s   .Lbad
        add.l   #0x1000,%d1
        cmp.l   #0x00f61000,%d1
        bne.s   .Lpage
        addq.l  #4,%d5
        cmp.l   #5,%d5
        beq.s   .Lmode
        moveq   #1,%d0
        bra.s   .Ldone
.Lbad:
        moveq   #0,%d0
.Ldone:
        movec   %a2,%vbr
        movec   %d4,%dfc
        movem.l (%sp)+,%d2-%d5/%a2-%a3
        rts
.Lfault:
        /* RTE consumes either the access-error or illegal-instruction frame.
         * Change only its PC to return to the common cleanup path.
         */
        move.l  #.Lbad,2(%sp)
        rte
        .bss
        .balign 4
.Lvectors:
        .space 1024
