******************************************************************************
*
* Kickstart ROM Module Wrapper for scsi710.device / scsi770.device
* Copyright 2025 Stefan Reinauer
*
* This program makes the non-rommable 'scsi7x0.device' a bootable,
* resident module suitable for inclusion in a Kickstart ROM.
*
* How it works:
* 1. A standard Resident structure ('res') makes this wrapper visible
*    to the OS during the boot sequence.
* 2. The OS calls the 'init_wrapper' routine via the RTF_COLDSTART flag.
* 3. 'init_wrapper' calls the hunk relocator ('reloc.S') to load
*    the embedded scsi7x0.device from ROM into RAM.
* 4. Once the device is relocated and live in RAM, the wrapper calls
*    its main entry point.
* 5. The scsi7x0.device's own code then takes over. As a bonus, we now
*    run the device driver from (hopefully FAST) RAM, avoiding slow ROM
*    accesses.
*
******************************************************************************

    INCLUDE "exec/types.i"
    INCLUDE "exec/resident.i"
    INCLUDE "exec/libraries.i"
    INCLUDE "exec/memory.i"
    INCLUDE "lvo/exec_lib.i"
    INCLUDE "version.i"

******* Resident Structure *******************************************
**********************************************************************
* This makes the wrapper visible to the OS at boot time.
*
    rts
res:
    dc.w    RTC_MATCHWORD   ; Magic cookie $4AFC
    dc.l    res             ; Pointer to self
    dc.l    end_wrapper     ; Pointer to the end of this module
    dc.b    RTF_COLDSTART   ; OS will call RT_INIT automatically
    dc.b    42              ; Version
    dc.b    NT_DEVICE       ; Node type
    dc.b    10              ; Priority for boot devices
    dc.l    name_string     ; Pointer to the name
    dc.l    id_string       ; Pointer to the ID string
    dc.l    init_wrapper    ; Pointer to our initialization code

name_string:
    IFD NCR53C710
    dc.b    'scsi710.device',0
    ENDIF
    IFD NCR53C770
    dc.b    'scsi770.device',0
    ENDIF

id_string:
    IFD NCR53C710
    dc.b    'A4000T_710_scsidisk '
    ENDIF
    IFD NCR53C770
    dc.b    'A4000T_770_scsidisk '
    ENDIF
    FULL_VERSION_STR
    dc.b    ' ('
    DATE_STR
    dc.b    ')',0
    CNOP    0,4             ; Word-align the following code

******* Wrapper Initialization Code **********************************
**********************************************************************

init_wrapper:
    movem.l d1-d7/a0-a6,-(sp)    ; Save all registers
    move.l  4.w,a6               ; Get ExecBase into a6

    lea     device_data(pc),a0   ; a0 -> pointer to the embedded binary
    moveq   #0,d0                ; d0 = 0 for memory-based source

    link    a4,#FP_SIZE
    bsr     _relocate            ; Call the relocator
    unlk    a4

    tst.l   d0
    beq.s   .fail                ; If d0 is 0, relocation failed

    ; Relocator was successful. d0 contains the SegList
    move.l  d0,a5

    ; --- Find the device's internal Resident tag within the new segment ---
    move.l  a5,a1                ; Use a1 for scanning
    addq.l  #4,a1                ; Skip the SegList's next-pointer

.find_res:
    cmp.w   #RTC_MATCHWORD,(a1)
    beq.s   .found_res
    addq.l  #2,a1
    bra.s   .find_res

.found_res:
    ; a1 now points to the internal Resident tag of scsi7x0.device
    move.l  RT_INIT(a1),a2       ; Get its RT_INIT function pointer

    tst.l   a2
    beq.s   .fail                ; Fail if there is no init routine

    ; Call the device's REAL init function.
    move.l  #0,a0
    jsr     (a2)

.fail:
    movem.l (sp)+,d1-d7/a0-a6    ; Restore registers
    rts                          ; Return to the OS

******* Relocator and RNC Decompressor Code **************************
**********************************************************************

ROM             EQU    1
    INCLUDE "reloc.S"


******* Embedded Device Data  ****************************************
**********************************************************************
* Use compressed version of the device driver to save space in ROM
*

device_data:
    IFD NCR53C710
    incbin "scsi710.device.zx0"
    ENDIF

    IFD NCR53C770
    incbin "scsi770.device.zx0"
    ENDIF

    CNOP    0,4

end_wrapper:
    END
