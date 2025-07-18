        INCLUDE "exec/memory.i"
        INCLUDE "lvo/exec_lib.i"
        INCLUDE "dos/doshunks.i"

        IFND ROM
        section .text,code
        ENDIF

NUM_ENTRIES EQU 5

; Frame pointer offsets
PHUNKS             EQU -4
READHANDLE_BASE    EQU -8
READHANDLE_TYPE    EQU -12
READHANDLE_CURRENT EQU -16
FP_SIZE            EQU READHANDLE_CURRENT

        ; API:
        ; Z3 ROM access:
        ;   a0: rombase
        ;   d0: offset
        ; Memory (RAM) access:
        ;   a0: pointer to binary
        ;   d0: 0
        public  _relocate
_relocate
        link    a4,#FP_SIZE     ; Allocate Frame pointer

        bsr     InitHandle

        ; fetch file header
        bsr     RomFetch32
        cmp.l   #$524e4301,d0 ; Are we compressed?
        beq.b   .decompress

        bsr     .real_relocate
        unlk    a4              ; Deallocate Frame pointer
        rts

.decompress
        movem.l d1-d7/a0-a6,-(sp)

        bsr RomFetch32
        ; d0 = original length
        move.l  d0,d3
        moveq.l #MEMF_PUBLIC,d1
        move.l  4,a6 ; execbase
        jsr     _LVOAllocMem(a6)
        move.l  d0,d2
        ; d2 / d3 = Original Ptr/Size

        bsr RomFetch32
        add.l   #18,d0 ; magic 18 bytes at the beginning
        move.l  d0,d5
        addq.l  #3,d0  ; padding
        and.b   #-4,d0 ; align
        moveq.l #MEMF_PUBLIC,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,d4
        ; d4 / d5 = Compressed Ptr/Size

        ; Reset ReadHandle (copy current state to frame)
        move.l  READHANDLE_CURRENT(a4),d0    ; Get current address
        tst.l   READHANDLE_TYPE(a4)
        beq     .direct
        sub.l   #48,d0 ; 12 bytes
        bra     .storeHandle
.direct
        sub.l   #12,d0 ; 12 bytes
.storeHandle
        move.l  d0,READHANDLE_CURRENT(a4)

        ; Copy compressed file to ram.
        move.l  d4,a0
        move.l  d5,d1
        subq.l  #1,d1 ; round down
        lsr.l   #2,d1
.Copy
        bsr     RomFetch32
        move.l  d0,(a0)
        addq.l  #4,a0
        dbra    d1,.Copy

        move.l  d4,a0
        move.l  d2,a1
        bsr     _Unpack

        move.l  d4,a1
        move.l  d5,d0
        addq.l  #3,d0  ; padding
        and.b   #-4,d0 ; align
        jsr     _LVOFreeMem(a6)

        move.l  d2,READHANDLE_CURRENT(a4)     ; memory location passed in a0
        move.l  #0,READHANDLE_TYPE(a4)  ; Set access type: 0 = memory, Z3 otherwise
        move.l  d2,READHANDLE_BASE(a4)  ; Set base address

        bsr     RomFetch32

        bsr     .real_relocate
        ; save return value
        move.l  d0,d4

        move.l  d2,a1
        move.l  d3,d0
        jsr     _LVOFreeMem(a6)

        move.l  d4,d0
        movem.l (sp)+,d1-d7/a0-a6
        unlk    a4                     ; Deallocate Frame pointer
        rts

.real_relocate
        movem.l d1-d7/a0-a6,-(sp)

        cmp.l   #HUNK_HEADER,d0 ; We only know hunk_hdr
        bne     .RelocateFail

        ; Allocate pHunks array
        moveq.l #(8*NUM_ENTRIES),d0
        move.l  #MEMF_PUBLIC|MEMF_CLEAR,d1
        jsr     _LVOAllocMem(a6)
        move.l  d0,PHUNKS(a4)

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

        move.l  PHUNKS(a4),a2   ; segment pointers
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


        ; a5: pointer to the pHunks table
        ; a0: set to pHunks[x] (segment pointer for current hunk)
        ; d0: contains hunk type
        ; d6: outer loop counter
        ; pHunks and pHunks[x+NUM] hold pointer + length

        move.l  PHUNKS(a4),a5
.HunkLoop
        move.l  (a5),a0 ; current hunk
        bsr     RomFetch32

        cmp.l   #HUNK_CODE,d0
        beq     .HunkCode
        cmp.l   #HUNK_DATA,d0
        beq     .HunkData
        cmp.l   #HUNK_BSS,d0
        beq     .HunkBSS
        cmp.l   #HUNK_RELOC32,d0
        beq     .HunkReloc32
        cmp.l   #HUNK_RELOC32SHORT,d0
        beq     .HunkReloc32Short
        cmp.l   #HUNK_DREL32,d0
        beq     .HunkDRel32
        cmp.l   #HUNK_SYMBOL,d0
        beq     .HunkSymbol
        cmp.l   #HUNK_RELRELOC32,d0
        beq     .HunkRelReloc32
        cmp.l   #HUNK_NAME,d0
        beq     .HunkName
        cmp.l   #HUNK_DEBUG,d0
        beq     .HunkDebug
        cmp.l   #HUNK_END,d0
        beq     .HunkEnd

        ; We don't know this hunk
.RelocateFail
        IFD HAVE_ERRNO
        ; Save hunk number in rErrno as error code
        lea     _rErrno(pc),a0
        move.l  d0,(a0)
        ENDIF
        bsr     .RelocateCleanup
        moveq.l #0,d0 ; NULL = Failure
        movem.l (sp)+,d1-d7/a0-a6
        rts

; -----------------------------------------------------
; The following functions parse the various hunk types
;
; Inputs:
;  a0: destination address
;  d0: hunk type
; Must not corrupt:
;  d6: .HunkLoop Counter
;  a5: pointer to current segment entry in table
;  a6: ExecBase
; -----------------------------------------------------

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
        move.l  PHUNKS(a4),a2
        add.l   d2,a2 ; hunk number num
        move.l  (a2),d3 ; base address of hunk

.Reloc32Tight
        bsr     RomFetch32
        ; get baseaddr + d0, add d3 to the data there.
        move.l  a0,a3
        add.l   d0,a3
        add.l   d3,(a3)
        dbra    d1,.Reloc32Tight

        bra     .HunkReloc32
; ---------------------

.HunkDRel32
.HunkReloc32Short
        bsr     RomFetch16

        tst.l   d0     ; if len is zero the reloc32 block is done
        beq     .HunkLoopPrep

        move.l  d0,d1 ; len ; number of offsets for a given hunk
        subq.l  #1,d1 ; for dbra

        bsr     RomFetch16
        move.l  d0,d2 ; num ; number of the hunk the offsets are to point into

        lsl.l   #2,d2 ; *4 ; offset from Hunks
        move.l  PHUNKS(a4),a2
        add.l   d2,a2 ; hunk number num
        move.l  (a2),d3 ; base address of hunk

.Reloc32ShortTight
        bsr     RomFetch16
        ; get baseaddr + d0, add d3 to the data there.
        move.l  a0,a3
        add.l   d0,a3
        add.l   d3,(a3)
        dbra    d1,.Reloc32ShortTight

        bra     .HunkReloc32Short
.HunkLoopPrep
        move.l  READHANDLE_CURRENT(a4),d0 ; align pointer to longword
        addq.l  #2,d0   ; in case we had an odd number
        and.l   #$fffffffc,d0  ; of relocations
        move.l  d0,READHANDLE_CURRENT(a4)
        bra     .HunkLoop
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
.HunkRelReloc32
        bsr     RomFetch32    ; d0 = number of relocations
        tst.l   d0
        beq     .HunkLoop     ; done

        move.l  d0,d1         ; relocation count
        subq.l  #1,d1         ; for dbra

        bsr     RomFetch32    ; target hunk index
        move.l  d0,d2
        lsl.l   #2,d2         ; word offset to entry
        move.l  PHUNKS(a4),a2
        add.l   d2,a2
        move.l  (a2),a3

        move.l  a0,a2

.RelRelocDo
        bsr     RomFetch32
        move.l  a2,a1
        add.l   d0,a1
        move.l  (a1),d3
        sub.l   a2,d3
        sub.l   d0,d3
        add.l   a3,d3
        move.l  d3,(a1)
        dbra    d1,.RelRelocDo
        bra     .HunkRelReloc32

; ---------------------

.HunkDebug
.HunkName
        bsr     RomFetch32 ; d0 = n longs
        tst.l   d0
        beq     .HunkLoop
        subq.l  #1,d0
.HunkDebugLoop
        bsr     RomFetch32
        dbra    d0,.HunkDebugLoop
        bra     .HunkLoop

; ---------------------

.HunkEnd
        addq.l  #4,a5
        dbra    d6,.HunkLoop
        IFD HAVE_ERRNO
        lea     _rErrno(pc),a0
        move.l  #0,(a0)
        ENDIF

        bsr.s   CreateSegList

        move.l  PHUNKS(a4),a0
        move.l  (a0),d2
        subq.l  #4,d2

        bsr.s   .RelocateCleanup
        move.l  d2,d0
        movem.l (sp)+,d1-d7/a0-a6
        rts

.RelocateCleanup
        move.l  PHUNKS(a4),a1
        moveq.l #(8*NUM_ENTRIES),d0
        jsr     _LVOFreeMem(a6)
        rts

; ---------------------

; +--------------------+
; | seg length (longs) | <-- = pHunks[x+NUM_ENTRIES]
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
        move.l  PHUNKS(a4),a0

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
        rts

; ---------------------

; data = RomFetch32At(offset)
; d0                  d0
RomFetch32At:
        movem.l a0-a1,-(sp)

        move.l  READHANDLE_BASE(a4),a0 ; base address
        lsl.l #2,d0
        add.l d0,a0
        move.l  a0,READHANDLE_CURRENT(a4) ; base address
        movem.l (sp)+,a0-a1
        bra RomFetch32

; data = RomFetch32(void)
; d0
RomFetch16
        moveq #1,d0
        bra.s RomFetch
RomFetch32
        moveq #3,d0

RomFetch
        movem.l a0-a1/d1-d3,-(sp)
        move.l  READHANDLE_CURRENT(a4),a0

        tst.l   READHANDLE_TYPE(a4)      ; access type ZorroIII?
        bne.s   .RomFetchZ3

        cmp.b   #3,d0
        bne.b   .RomFetchWord
        move.l  (a0)+,d0   ; access type is memory
        bra.s   .RomFetchDone
.RomFetchWord
        move.w  (a0)+,d0
        bra.s   .RomFetchDone

.RomFetchZ3                ; access type is ZorroIII
        move.l  d0,d3
.nextnip
        lsl.l   #8,d0
        move.b  (a0),d1
        and.l   #$f0,d1
        move.b  2(a0),d2
        and.l   #$f0,d2
        lsr.w   #4,d2
        or.b    d1,d2

        move.b  d2,d0

        addq    #4,a0
        dbra    d3,.nextnip

.RomFetchDone
        move.l  a0,READHANDLE_CURRENT(a4)
        movem.l (sp)+,a0-a1/d1-d3
        rts

InitHandle
        ; initialize readhandle to beginning of device driver
        ; ROM_OFFSET needs to be multiplied by 4 because of the
        ; nibble mapped nature of the AutoConfig ROM.
        ; ROM_ADDRESS is passed in a0
        ; ROM_OFFSET is passed in d0
        movem.l a0-a1,-(sp)
        lsl.l   #2,d0

        move.l  a0,READHANDLE_BASE(a4) ; base address
        add.l   d0,a0
        move.l  a0,READHANDLE_CURRENT(a4)   ; memory location passed in a0
        move.l  d0,READHANDLE_TYPE(a4) ; Set access type: 0 = memory, Z3 otherwise

        movem.l (sp)+,a0-a1
        rts

        IFD HAVE_ERRNO
        public _rErrno
_rErrno
        dc.l    0
        ENDIF
        INCLUDE "rnc.S"
        CODE
