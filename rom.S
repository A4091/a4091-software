**
** A4091 ROM based on
** Sample autoboot code fragment
**

        INCLUDE "exec/types.i"
        INCLUDE "exec/nodes.i"
        INCLUDE "exec/resident.i"
        INCLUDE "lvo/exec_lib.i"
        INCLUDE "libraries/configvars.i"
        INCLUDE "version.i"

*----- We'll store Version and Revision in serial number
VERSION         EQU     DEVICE_VERSION  ; also the high word of serial number
REVISION        EQU     DEVICE_REVISION ; also the low word of serial number

* See the Amiga Hardware Manual for more info.

MANUF_ID        EQU    514        ; Commodore West Chester
PRODUCT_ID      EQU     84        ; A4091

ZORRO_III      EQU     $80
AUTOBOOT_ROM   EQU     $10
BOARDSIZE_16MB EQU     $00

ER_TYPE        EQU     (ZORRO_III|AUTOBOOT_ROM|BOARDSIZE_16MB)

ZORRO_III_IO   EQU     $00
ZORRO_III_MEM  EQU     $80
ZORRO_III_EXT  EQU     $20
ZORRO_III_RSVD EQU     $10

ER_FLAGS       EQU    (ZORRO_III_IO|ZORRO_III_EXT|ZORRO_III_RSVD)

ROM_VECTOR     EQU     $0200      ; $80 in the ROM maps to $200 in the system

        CODE


* Define DRIVER_BOOT_CRASH_DEBUG to enable debug where driver will
* "fail" to load 1 out of 4 tries. It writes specific registers to
* fixed Physical addresses in fast memory. These are:
*   0x07770000 The try count (only the bottom two bits matter here)
*   0x07770004 A0 board base address
*   0x07770008 A1
*   0x0777000c A2 Diag/init base address
*   0x07770010 A3 board configdev structure
*   0x07770014 A4
*   0x07770018 A5
*   0x0777001c A6
* DRIVER_BOOT_CRASH_DEBUG EQU 1


*******  RomStart  ***************************************************
**********************************************************************

RomStart
        *   ExpansionRom structure
        *
        *   On the A4091 the AUTOCONFIG ROM information is encoded in nibbles across
        *   two 128 byte arrays.

        * er_Type is NOT inverted
        dc.b    ER_TYPE|$0f, ~(PRODUCT_ID)|$0f, ~(ER_FLAGS), $ff
        dc.b    (~(MANUF_ID)|$0f)&$ff, (~(MANUF_ID>>8)|$0f)&$ff, ~(VERSION>>8), ~(VERSION)
        dc.b    ~(REVISION>>8), ~(REVISION), ~(ROM_VECTOR>>8)|$0f, ~(ROM_VECTOR)&$ff
        dc.b    $ff, $ff, $ff, $ff
        dc.b    $ff, $ff, $ff, $ff, $ff, $ff, $ff, $ff
        dc.b    $ff, $ff, $ff, $ff, $ff, $ff, $ff, $ff
        dc.b    $ff, $ff, $ff, $ff, $ff, $ff, $ff, $ff
        dc.b    $ff, $ff, $ff, $ff, $ff, $ff, $ff, $ff
        dc.b    $ff, $ff, $ff, $ff, $ff, $ff, $ff, $ff
        dc.b    $ff, $ff, $ff, $ff, $ff, $ff, $ff, $ff

        IFNE    *-RomStart-$40
        FAIL    "Even half of ExpansionRom structure not the right size"
        ENDC

        dc.b    ((ER_TYPE<<4)|$0f)&$ff, ~(PRODUCT_ID<<4)&$ff, ~(ER_FLAGS<<4)&$ff, $ff
        dc.b    ~(MANUF_ID>>4), ~(MANUF_ID<<4)&$ff, ~(VERSION>>4), ~(VERSION<<4)&$ff
        dc.b    ~(REVISION>>4), ~(REVISION<<4)&$ff, ~(ROM_VECTOR>>4), ~(ROM_VECTOR<<4)&$ff
        dc.b    $ff, $ff, $ff, $ff
        dc.b    $ff, $ff, $ff, $ff, $ff, $ff, $ff, $ff
        dc.b    $ff, $ff, $ff, $ff, $ff, $ff, $ff, $ff
        dc.b    $ff, $ff, $ff, $ff, $ff, $ff, $ff, $ff
        dc.b    $ff, $ff, $ff, $ff, $ff, $ff, $ff, $ff
        dc.b    $ff, $ff, $ff, $ff, $ff, $ff, $ff, $ff
        dc.b    $ff, $ff, $ff, $ff, $ff, $ff, $ff, $ff

        IFNE    *-RomStart-$80
        FAIL    "Odd half of ExpansionROM structure not the right size"
        ENDC

*******  DiagStart  **************************************************
DiagStart
        ; This is the DiagArea structure whose relative offset from
        ; your board base appears as the Init Diag vector in your
        ; autoconfig ID information.  This structure is designed
        ; to use all relative pointers (no patching needed).
        dc.b    DAC_NIBBLEWIDE+DAC_CONFIGTIME  ; da_Config
        dc.b    0                              ; da_Flags
        dc.w    EndCopy-DiagStart              ; da_Size
        dc.w    DiagEntry-DiagStart            ; da_DiagPoint
        dc.w    BootEntry-DiagStart            ; da_BootPoint
        dc.w    DevName-DiagStart              ; da_Name
        dc.w    0                              ; da_Reserved01
        dc.w    0                              ; da_Reserved02

*******  DiagEntry  **************************************************
**********************************************************************
*
*   success = DiagEntry(BoardBase,DiagCopy, configDev)
*   d0                  a0         a2       a3
*
*   Called by expansion architecture to relocate any pointers
*   in the copied diagnostic area.   We will patch the romtag.
*   If you have pre-coded your MakeDosNode packet, BootNode,
*   or device initialization structures, they would also need
*   to be within this copy area, and patched by this routine.
*
**********************************************************************
* These are the calling conventions for the Diag routine
*
* A7 -- points to at least 2K of stack
* A6 -- ExecBase
* A5 -- ExpansionBase
* A3 -- your board's ConfigDev structure
* A2 -- Base of diag/init area that was copied
* A0 -- Base of your board
*
* The Diag routine shall return a non-zero value in D0 for success.
* If this value is NULL, then the diag/init area that was copied
* will be returned to the free memory pool.
*

DiagEntry
        movem.l d2-d7/a2-a6,-(sp)

        ; Patch up Romtag Resident Structure in memory
        lea.l   Romtag(pc),a4                ; find Romtag in memory
        move.l  a4,RT_MATCHTAG(a4)           ; pointer to itself
        lea.l   EndCopy(pc),a1
        move.l  a1,RT_ENDSKIP(a4)
        lea.l   DevName(pc),a1
        move.l  a1,RT_NAME(a4)
        lea.l   IdString(pc),a1
        move.l  a1,RT_IDSTRING(a4)

        ;----------------------------------------------------------
        ; Check ROM for a4091.device. If it is not there, this
        ; is a nodevice rom
        ;----------------------------------------------------------
        move.l  #(_device-RomStart),d0
        link    a4,#FP_SIZE     ; Allocate ReadHandle frame pointer
        bsr InitHandle

        move.l #(65536-4),d0
        bsr RomFetch32At
        cmp.l #$2f434448,d0
        bne .try32

        ; 64k rom
        move.l #(65536-12),d0 ; devicelen
        bsr RomFetch32At
        tst.l d0 ; No device len?
        bne .gorelocate

        bra .no_resident

.try32
        move.l #(32768-4),d0
        bsr RomFetch32At
        cmp.l #$2f434448,d0
        bne .gorelocate ; not 32 or 64k, you're on your own.

	; 32k rom
        move.l #(32768-12),d0 ; devicelen
        bsr RomFetch32At
        tst.l d0 ; No device len?
        bne .gorelocate

.no_resident
        unlk    a4      ; deallocate ReadHandle
        ; destroy memory romtag
        lea.l   Romtag(pc),a4
        move.l  #0,(a4)

        bra .done

.gorelocate
	;----------------------------------------------------------

        unlk    a4      ; deallocate ReadHandle
	; a0 - base of your board = RomStart?
        move.l  #(_device-RomStart),d0
        bsr     _relocate
        tst.l   d0
        bne.s   .ok

        *******  Show checkered purple failure screen **************************
        movem.l d2-d3,-(sp)
        move.w  #$0020,d2
.fc1
        move.w  #$ffff,d3
.fc2
        move.w  #$0000,$dff180               ; black
        move.w  #$0f0c,$dff180               ; purple
        dbra    d3,.fc2
        dbra    d2,.fc1
        movem.l (sp)+,d2-d3
        *******  End checkered purple failure screen **************************

        moveq.l #0,d0
        movem.l (sp)+,d2-d7/a2-a6
        rts
.ok
        lea.l   Romtag(pc),a4                ; find Romtag in memory
        move.l  d0,a2
        addq.l  #4,a2

        cmp.w   #$4afc,4(a2)
        beq.s   .new_style
.old_style
        move.l  a2,RT_INIT(a4)
        bra.s   .done
.new_style
        add.l   #4+RT_INIT,a2 ; skip _start and find _auto_init_tables
        move.l  (a2),RT_INIT(a4)

.done
        movem.l (sp)+,d2-d7/a2-a6

        moveq.l #1,d0           ; indicate "success"

        IFD DRIVER_BOOT_CRASH_DEBUG
        *******  Debug driver boot crash *********
        movem.l a0/d1,-(sp)
        move.l  #_device,d1
        move.l  a0,d1
        move.l  #$07770020,a0
        movem.l d1/a1-a6,-(a0)
        subi    #1,-(a0)
        move.l  (a0),d1
        and.l   #$03,d1
        bne     .ok_to_boot
        moveq.l #0,d0           ; indicate "fail"
.ok_to_boot
        movem.l (sp)+,a0/d1
        ENDIF
        rts

*******  BootEntry  **************************************************
**********************************************************************

BootEntry
        lea     DosName(PC),a1          ; 'dos.library',0
        jsr     _LVOFindResident(a6)    ; find the DOS resident tag
        tst.l   d0
        beq.s   .BadBoot
        move.l  d0,a0                   ; in order to bootstrap
        move.l  RT_INIT(A0),a0          ; set vector to DOS INIT
        jsr     (a0)                    ; and initialize DOS
.BadBoot
        rts

*******  Resident Structure  *****************************************
**********************************************************************
Romtag
        dc.w    RTC_MATCHWORD      ; UWORD RT_MATCHWORD
        dc.l    0                  ; APTR  RT_MATCHTAG
        dc.l    0                  ; APTR  RT_ENDSKIP
        dc.b    RTF_COLDSTART      ; UBYTE RT_FLAGS
        dc.b    VERSION            ; UBYTE RT_VERSION
        dc.b    NT_DEVICE          ; UBYTE RT_TYPE
        dc.b    10                 ; BYTE  RT_PRI
        dc.l    0                  ; APTR  RT_NAME
        dc.l    0                  ; APTR  RT_IDSTRING
        dc.l    0                  ; APTR  RT_INIT

        ; After Diag patching, our romtag will point its Init pointer
        ; to the auto_init_tables of a4091.device so that it can be
        ; called at Resident initialization time.

*******  Strings referenced in Diag Copy area  **************************
*************************************************************************
DevName
        IFD DRIVER_A4092
        dc.b    'a4092.device',0                   ; Name string
        ELSE
        dc.b    'a4091.device',0                   ; Name string
        ENDC
IdString
        IFD DRIVER_A4092
        dc.b    'A4092 scsidisk '
        ELSE
        dc.b    'A4091 scsidisk '
        ENDC
        FULL_VERSION_STR
        dc.b    0                     ; ID string
DosName
        dc.b    'dos.library',0                    ; DOS library name
        align 1     ; word align

*******  Relocation Code  ***********************************************
*************************************************************************
ROM             EQU    1
        INCLUDE "reloc.S"
*
* End of the Diag copy area which is copied to RAM
*
EndCopy

        ; keep out of the Diag area copied to RAM automatically
        section DEVICE
        public  _device
_device

*************************************************************************
        END
