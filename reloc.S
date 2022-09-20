        INCLUDE "exec/memory.i"
        INCLUDE "lvo/exec_lib.i"

        IFND ROM
        section .text,code
        ENDIF

NUM_ENTRIES EQU 5

        ; API:
        ; Z3 ROM access:
        ;   a0: rombase
        ;   d0: offset
        ; Memory (RAM) access:
        ;   a0: pointer to binary
        ;   d0: 0
        public  _relocate
_relocate
        bsr     InitHandle

        ; fetch file header
        bsr     RomFetch32
        cmp.l   #$3f3,d0 ; We only know hunk_hdr
        bne     .RelocateFail

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

        move.l  pHunks(pc),a2   ; segment pointers
        lea.l   4*NUM_ENTRIES(a2),a3        ; segment lengths
.AllocateLoop
        moveq.l #MEMF_PUBLIC,d1
        bsr     RomFetch32
        lsl.l   #2,d0
        move.l  d0,(a3)+         ; segment length
        addq.l  #8,d0            ; Add 8 for seglist overhead
        jsr     _LVOAllocMem(a6)
        tst.l   d0
        beq.s   .RelocateFail
        addq.l  #8,d0            ; Skip seglist overhead
        move.l  d0,(a2)+         ; segment pointer
        dbra    d7,.AllocateLoop

        move.l  pHunks(pc),a5
.HunkLoop
        move.l  (a5),a0 ; current hunk
        bsr     RomFetch32

        cmp.l   #$3e9,d0
        beq     .HunkCode
        cmp.l   #$3ea,d0
        beq     .HunkData
        cmp.l   #$3eb,d0
        beq     .HunkBSS
        cmp.l   #$3ec,d0
        beq     .HunkReloc32
        cmp.l   #$3f0,d0
        beq     .HunkSymbol
        cmp.l   #$3f2,d0
        beq     .HunkEnd

        ; We don't know this hunk
.RelocateFail
        ; Save hunk number in rErrno as error code
        lea     _rErrno(pc),a0
        move.l  d0,(a0)
        bsr     .RelocateCleanup
        moveq.l #0,d0 ; NULL = Failure
        rts

; ---------------------

.HunkData
.HunkCode
        bsr     RomFetch32
        move.l  d0,d7
        subq.l  #1,d7
.CopyHunk
        bsr     RomFetch32
        move.l  d0,(a0)+
        dbra    d7,.CopyHunk
        bra     .HunkLoop

; ---------------------

.HunkBSS
        bsr     RomFetch32
        move.l  d0,d7
        subq.l  #1,d7
        ; hunk address in a0
.ClearBSS
        move.l  #$0,(a0)+
        dbra    d7,.ClearBSS

        bra     .HunkLoop

; ---------------------

.HunkReloc32
        bsr     RomFetch32

        tst.l   d0     ; if len is zero the reloc32 block is done
        beq     .HunkLoop

        move.l  d0,d1 ; len ; number of offsets for a given hunk
        subq.l  #1,d1 ; for dbra

        bsr     RomFetch32
        move.l  d0,d2 ; num ; number of the hunk the offsets are to point into

        lsl.l   #2,d2 ; *4 ; offset from Hunks
        move.l  pHunks(pc),a2
        add.l   d2,a2 ; hunk number num
        move.l  (a2),d3 ; base address of hunk

.RelocTight
        bsr     RomFetch32
        ; get baseaddr + d0, add d3 to the data there.
        move.l  a0,a4
        add.l   d0,a4
        add.l   d3,(a4)
        dbra    d1,.RelocTight

        bra     .HunkReloc32

; ---------------------

.HunkSymbol
        bsr     RomFetch32
        tst.l   d0
        beq     .HunkLoop
        move.l  d0,d7
.SkipSymbol
        bsr     RomFetch32
        dbra    d7,.SkipSymbol
        bra     .HunkSymbol

; ---------------------

.HunkEnd
        addq.l  #4,a5
        dbra    d6,.HunkLoop

        lea     _rErrno(pc),a0
        moveq.l #0,d0
        move.l  d0,(a0)

        bsr.s   CreateSegList
        move.l  pHunks(pc),a0
        move.l  (a0),d2
        subq.l  #4,d2
        bsr.s   .RelocateCleanup
        move.l  d2,d0
        rts

.RelocateCleanup
        move.l  pHunks(pc),a1
        moveq.l #(8*NUM_ENTRIES),d0
        jsr     _LVOFreeMem(a6)
        rts

; ---------------------

; +--------------------+
; | seg length (longs) | <-- = pHunks[x+4]
; +--------------------+ <-- seglist returned
; | bptr to next seg   | <-- = pHunks[x+1]
; +--------------------+ <-- pHunks[x] points here
; | segment data (also |
; | first entry point) |
; |/\/\/\/\/\/\/\/\/\/\|

; pHunks  -+  Hunk Pointers Hunk Lengths
;          |  +--+--+--+--+ +--+--+--+--+
;          +->|  |  |  | 0| |  |  |  | 0|
;             +--+--+--+--+ +--+--+--+--+
CreateSegList:
        move.l  pHunks(pc),a0

.NextSeg
        move.l  (a0),a1     ; Hunks[x]

        move.l  4*NUM_ENTRIES(a0),d0  ; length of current hunk
        addq.l  #8,d0       ; add header size
        lsr.l   #2,d0       ; MKBADDR
        move.l  d0,-8(a1)   ; write seg size

        addq.l  #4,a0
        move.l  (a0),d0    ; next hunk
        tst.l   d0          ; hunk addr zero?
        beq     .SegListDone ; we're done
        subq.l  #4,d0       ; else: point to linked list
        lsr.l   #2,d0       ; MKBADDR
        move.l  d0,-4(a1)   ; write BPTR to next hunk

        bra.s   .NextSeg

.SegListDone
        move.l  d0,-4(a1)   ; terminate seglist
        move.l  (a0),a1
        subq.l  #4,a1
        rts

; ---------------------

; data = RomFetch32(void)
; d0
RomFetch32
        movem.l a0-a1/d1-d3,-(sp)
        lea     ReadHandle(pc),a1
        move.l  (a1),a0

        tst.l   4(a1)      ; access type ZorroIII?
        bne.s   .RomFetchZ3

        move.l  (a0)+,d0   ; access type is memory
        bra.s   .RomFetchDone

.RomFetchZ3                ; access type is ZorroIII
        moveq.l #3,d3
.nextnip
        lsl.l   #8,d0
        move.b  (a0),d1
        and.b   #$f0,d1
        move.b  2(a0),d2
        lsr.w   #4,d2
        or.b    d1,d2

        move.b  d2,d0

        addq    #4,a0
        dbra    d3,.nextnip

.RomFetchDone
        move.l  a0,(a1)
        movem.l (sp)+,a0-a1/d1-d3
        rts

InitHandle
        ; initialize readhandle to beginning of device driver
        ; ROM_OFFSET needs to be multiplied by 4 because of the
        ; nibble mapped nature of the AutoConfig ROM.
        ; ROM_OFFSET is passed in d0
        lea     ReadHandle(pc),a2

        lsl.l   #2,d0

        lea     (a0,d0),a1
        move.l  a1,(a2)  ; memory location passed in a0
        move.l  d0,4(a2) ; Set access type: 0 = memory, Z3 otherwise

        moveq.l #(8*NUM_ENTRIES),d0
        move.l  #MEMF_PUBLIC|MEMF_CLEAR,d1
        jsr     _LVOAllocMem(a6)
        lea     pHunks(pc),a2
        move.l  d0,(a2)
        rts

ReadHandle
        dc.l    0 ; Current address
        dc.l    0 ; 0=memory, otherwise z3

pHunks
        dc.l    0

        public _rErrno
_rErrno
        dc.l    0
        CODE
