;
; scsifix - redirect access to 2nd.scsi.device to a4091.device
;

; Exec
ExecBase		EQU	4

_LVOFindTask		EQU	-294
_LVOWait		EQU	-318
_LVOCloseLibrary	EQU	-414
_LVOSetFunction		EQU	-420
_LVOOpenLibrary		EQU	-552
_LVOOpenDevice		EQU	-444

LN_NAME			EQU     10
SIGBREAKB_CTRL_C        EQU     12

; Dos
_LVOWrite		EQU	-48
_LVOOutput		EQU	-60


	CODE
Start:
	move.l		#10,ErrorCode

	; Find myself
	suba.l		a1,a1
	movea.l		ExecBase.w,a6
	jsr		_LVOFindTask(a6)
	move.l		d0,MyTask

	; Open dos.library
	moveq.l		#0,d0
	lea		DosName(PC),a1
	movea.l		ExecBase.w,a6
	jsr		_LVOOpenLibrary(a6)
	move.l		d0,DosBase
	beq.w		.Fail

	; Find output stream handle
	movea.l		DosBase,a6
	jsr		_LVOOutput(a6)
	move.l		d1,OutHandle
	beq.w		.Fail2

	move.l		OutHandle(PC),d1
	move.l		#Banner,d2
	move.l		#_Banner-Banner,d3
	movea.l		DosBase,a6
	jsr		_LVOWrite(a6)

	lea		QFTaskName(PC),a1
	movea.l		ExecBase.w,a6
	jsr		_LVOFindTask(a6)
	tst.l		d0
	beq.b		.InstallFunction
	; task already there, we were already installed and active
	move.l		OutHandle(PC),d1
	move.l		#Active,d2
	move.l		#_Active-Active,d3
	movea.l		DosBase,a6
	jsr		_LVOWrite(a6)
	bra.w		.Fail2

.InstallFunction:
	movea.l		MyTask(PC),a0
	move.l		LN_NAME(a0),OldTaskName
	move.l		#QFTaskName,LN_NAME(a0)
	movea.l		ExecBase.w,a1
	lea		_LVOOpenDevice.w,a0
	move.l		#NewOpenDevice,d0
	movea.l		ExecBase.w,a6
	jsr		_LVOSetFunction(a6)
	move.l		d0,OldOpenDevice

	; Print installed message
	move.l		OutHandle(PC),d1
	move.l		#Installed,d2
	move.l		#_Installed-Installed,d3
	movea.l		DosBase,a6
	jsr		_LVOWrite(a6)

	; Wait for CTRL-C
	move.l		#(1<<SIGBREAKB_CTRL_C),d0
	movea.l		ExecBase.w,a6
	jsr		_LVOWait(a6)

	move.l		#0,ErrorCode
	; Print removed message
	move.l		OutHandle(PC),d1
	move.l		#Removed,d2
	move.l		#_Removed-Removed,d3
	movea.l		DosBase,a6
	jsr		_LVOWrite(a6)

	movea.l		ExecBase.w,a1
	lea		_LVOOpenDevice.w,a0
	move.l		OldOpenDevice(PC),d0
	movea.l		ExecBase.w,a6
	jsr		_LVOSetFunction(a6)

	movea.l		MyTask(PC),a0
	move.l		OldTaskName(PC),LN_NAME(a0)
.Fail2:	movea.l		DosBase(PC),a1
	movea.l		ExecBase.w,a6
	jsr		_LVOCloseLibrary(a6)
.Fail:	move.l		ErrorCode(PC),d0
	rts

NewOpenDevice:
	movem.l		d0/a0-a1,-(sp)
	lea		SCSIName(PC),a1
.IsSerial:
	move.b		(a0)+,d0
	cmp.b		(a1)+,d0
	bne.b		.DoOpenDevice
	tst.b		d0
	bne.b		.IsSerial
	move.l		#A4091Name,4(sp)
.DoOpenDevice:
	movem.l		(sp)+,d0/a0-a1
	move.l		OldOpenDevice(PC),-(sp)
	rts

MyTask:		dc.l		0
DosBase:	dc.l		0
OldTaskName:	dc.l		0
OldOpenDevice:	dc.l		0
OutHandle:	dc.l		0
ErrorCode:	dc.l		0

DosName:	dc.b		"dos.library",0
QFTaskName:	dc.b		"scsifix",0

SCSIName:	dc.b		"2nd.scsi.device",0
A4091Name:	dc.b		"a4091.device",0

Banner:		dc.b		"scsifix - Point 2nd.scsi.device to a4091.device",10
_Banner:
Active:		dc.b		"SCSI fix already active!",10
_Active:
Installed:	dc.b		"SCSI fix installed.",10
_Installed:
Removed:	dc.b		"SCSI fix removed.",10
_Removed:
