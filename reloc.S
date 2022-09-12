            INCLUDE "lvo/exec_lib.i"

MEMF_PUBLIC EQU 1

        ; a0: rombase
        ; d0: offset
        public  _relocate
_relocate:
        bsr     InitHandle

        ; fetch file header
        bsr     RomFetch32
        cmp.l   #$3f3,d0 ; We only know hunk_hdr
        bne     RelocateFail

        ; consume the header and remember number of hunks
        bsr     RomFetch32
        bsr     RomFetch32
        move.l  d0,d7 ; table size
        subq.l  #1,d7
        move.l  d7,d6 ; for HUNK_END
        bsr     RomFetch32
        bsr     RomFetch32

        ; Go through hunk header and allocate all segments
        move.l  4,a6 ; execbase
        lea.l   _Hunks(pc),a2
        ifd     USERLAND
        lea.l   _HunksLen(pc),a3	; debug only
        endif
AllocateLoop
        move.l  #MEMF_PUBLIC,d1
        bsr     RomFetch32
        lsl.l   #2,d0
        ifd     USERLAND
        move.l  d0,(a3)+ ; debug only
        endif
        jsr     _LVOAllocMem(a6)
        move.l  d0,(a2)+
        dbra    d7,AllocateLoop

        lea     _Hunks(pc),a5
HunkLoop
        move.l  (a5),a0 ; current hunk
        bsr     RomFetch32

        cmp.l   #$3e9,d0
        beq     HunkCode
        cmp.l   #$3ea,d0
        beq     HunkData
        cmp.l   #$3eb,d0
        beq     HunkBSS
        cmp.l   #$3ec,d0
        beq     HunkReloc32
        cmp.l   #$3f0,d0
        beq     HunkSymbol
        cmp.l   #$3f2,d0
        beq     HunkEnd

        ; We don't know this hunk
RelocateFail
        ; Just return d0 for now, it's more useful
        ; than -1
        ; move.l  #$ffffffff,d0
        rts

; ---------------------

HunkData
HunkCode
        bsr     RomFetch32
        move.l  d0,d7
        subq.l  #1,d7
CopyHunk
        bsr     RomFetch32
        move.l  d0,(a0)+
        dbra    d7,CopyHunk
        bra     HunkLoop

; ---------------------

HunkBSS
        bsr     RomFetch32
        move.l  d0,d7
        subq.l  #1,d7
        ; hunk address in a0
ClearBSS
        move.l  #$0,(a0)+
        dbra    d7,ClearBSS

        bra     HunkLoop

; ---------------------

HunkReloc32
        bsr     RomFetch32

        tst.l   d0     ; if len is zero the reloc32 block is done
        beq     HunkLoop

        move.l  d0,d1 ; len ; number of offsets for a given hunk
        subq.l  #1,d1 ; for dbra

        bsr     RomFetch32
        move.l  d0,d2 ; num ; number of the hunk the offsets are to point into

        lsl.l   #2,d2 ; *4 ; offset from Hunks
        lea     _Hunks(pc),a2
        add.l   d2,a2 ; hunk number num
        move.l  (a2),d3 ; base address of hunk

RelocTight
        bsr     RomFetch32
        ; get baseaddr + d0, add d3 to the data there.
        move.l  a0,a4
        add.l   d0,a4
        add.l   d3,(a4)
        dbra    d1,RelocTight

        bra     HunkReloc32

; ---------------------

HunkSymbol
        bsr     RomFetch32
        tst.l   d0
        beq     HunkLoop
        move.l  d0,d7
SkipSymbol
        bsr     RomFetch32
        dbra    d7,SkipSymbol
        bra     HunkSymbol

; ---------------------

HunkEnd
        addq.l  #4,a5
        dbra    d6,HunkLoop
        moveq.l #$0,d0
        rts

; ---------------------

; data = RomFetch32(void)
; d0
RomFetch32
        ifd     USERLAND
        movem.l a0-a1,-(sp)
        lea     ReadHandle(pc),a1
        move.l  (a1),a0
        move.l  (a0)+,d0
        move.l  a0,(a1)
        movem.l (sp)+,a0-a1
        rts
        endif
; ---------------------
RomFetchZ3
        movem.l a0-a1/d1-d3,-(sp)
        lea     ReadHandle(pc),a1
        move.l  (a1),a0

        moveq.l #3,d3
nextnip
        lsl.l   #8,d0
        move.b  (a0),d1
        and.b   #$f0,d1
        move.b  2(a0),d2
        lsr.w   #4,d2
        or.b    d1,d2

        move.b  d2,d0

        addq    #4,a0
        dbra    d3,nextnip
        move.l  a0,(a1)
        movem.l (sp)+,a0-a1/d1-d3
        rts

InitHandle
        ifd     USERLAND
        lea     device,a1 ; Not (pc) because different section
        lea     ReadHandle(pc),a2
        move.l  a1,(a2)
        rts
        endif
InitHandleZ3
        ; initialize readhandle to beginning of device driver
        ; ROM_OFFSET needs to be multiplied by 4 because of the
        ; nibble mapped nature of the AutoConfig ROM.
        ; ROM_OFFSET is passed in d0
        lsl.l   #2,d0
        lea     (a0,d0),a1
        lea     ReadHandle(pc),a2
        move.l  a1,(a2)
        rts

ReadHandle
        dc.l    0

        public  _Hunks
_Hunks
        dc.l    0,0,0,0

        ifd     USERLAND
        public  _HunksLen
_HunksLen
        dc.l    0,0,0,0
        endif

        ; keep out of the Diag area copied to RAM automatically
        section DEVICE,data
device:
        ifd COMMODORE_DEVICE
        incbin "a3090.ld_strip"
        else
        incbin "a4091.device"
        endif

        code
