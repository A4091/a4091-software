;	$NetBSD: siop_script.ss,v 1.6 2016/12/12 15:58:44 maya Exp $

;
; Copyright (c) 1995 Michael L. Hitch
; All rights reserved.
;
; Redistribution and use in source and binary forms, with or without
; modification, are permitted provided that the following conditions
; are met:
; 1. Redistributions of source code must retain the above copyright
;    notice, this list of conditions and the following disclaimer.
; 2. Redistributions in binary form must reproduce the above copyright
;    notice, this list of conditions and the following disclaimer in the
;    documentation and/or other materials provided with the distribution.
;
; THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
; IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
; OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
; IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
; INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
; NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
; DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
; THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
; (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
; THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
;

; NCR 53c710 script
;
ARCH 710
;
ABSOLUTE ds_Device	= 0
ABSOLUTE ds_MsgOut 	= ds_Device + 4
ABSOLUTE ds_Cmd		= ds_MsgOut + 8
ABSOLUTE ds_Status	= ds_Cmd + 8
ABSOLUTE ds_Msg		= ds_Status + 8
ABSOLUTE ds_MsgIn	= ds_Msg + 8
ABSOLUTE ds_ExtMsg	= ds_MsgIn + 8
ABSOLUTE ds_SyncMsg	= ds_ExtMsg + 8
ABSOLUTE ds_Data1	= ds_SyncMsg + 8
ABSOLUTE ds_Data2	= ds_Data1 + 8
ABSOLUTE ds_Data3	= ds_Data2 + 8
ABSOLUTE ds_Data4	= ds_Data3 + 8
ABSOLUTE ds_Data5	= ds_Data4 + 8
ABSOLUTE ds_Data6	= ds_Data5 + 8
ABSOLUTE ds_Data7	= ds_Data6 + 8
ABSOLUTE ds_Data8	= ds_Data7 + 8
ABSOLUTE ds_Data9	= ds_Data8 + 8

; Interrupt codes - returned to host driver via DSPS register
ABSOLUTE A_ok			= 0xff00	; Command completed successfully
ABSOLUTE A_int_disc		= 0xff01	; Disconnect (after Save Data Pointers)
ABSOLUTE A_int_disc_nosdp	= 0xff02	; Disconnect (without Save Data Pointers)
ABSOLUTE A_int_resel		= 0xff03	; Reselect/reconnect from target
ABSOLUTE A_int_sel_fail		= 0xff04	; Select failed (not connected)
ABSOLUTE A_int_bad_phase	= 0xff05	; Unrecognized SCSI phase
ABSOLUTE A_int_bad_msg		= 0xff06	; Unrecognized message received
ABSOLUTE A_int_extmsg_notsdtr	= 0xff07	; Extended message not SDTR
ABSOLUTE A_int_sdp_nodisc	= 0xff08	; Save Data Ptrs not followed by Disconnect
ABSOLUTE A_int_resel_no_id	= 0xff09	; Reselect without IDENTIFY message
ABSOLUTE A_int_status_no_msg	= 0xff0a	; Status not followed by message
ABSOLUTE A_int_sdtr		= 0xff0b	; SDTR message - let host handle

ENTRY	scripts
ENTRY	switch
ENTRY	wait_reselect
ENTRY	dataout
ENTRY	datain
ENTRY	clear_ack

PROC	scripts:

scripts:

	SELECT ATN FROM ds_Device, REL(reselect)
;
switch:
	JUMP REL(msgin), WHEN MSG_IN
	JUMP REL(msgout), IF MSG_OUT
	JUMP REL(command_phase), IF CMD
	JUMP REL(dataout), IF DATA_OUT
	JUMP REL(datain), IF DATA_IN
	JUMP REL(end), IF STATUS

	INT A_int_bad_phase		; Unrecognized phase

msgin:
	MOVE FROM ds_MsgIn, WHEN MSG_IN
	JUMP REL(ext_msg), IF 0x01	; extended message
	JUMP REL(disc), IF 0x04		; disconnect message
	JUMP REL(msg_sdp), IF 0x02	; save data pointers
	JUMP REL(msg_rej), IF 0x07	; message reject
	JUMP REL(msg_rdp), IF 0x03	; restore data pointers
	JUMP REL(clear_ack), IF 0x08	; NOP message
	INT A_int_bad_msg		; unrecognized message

msg_rej:
; Do we need to interrupt host here to let it handle the reject?
msg_rdp:
clear_ack:
	CLEAR ACK
	CLEAR ATN
	JUMP REL(switch)

ext_msg:
	CLEAR ACK
	MOVE FROM ds_ExtMsg, WHEN MSG_IN
	JUMP REL(sync_msg), IF 0x03
	INT A_int_extmsg_notsdtr	; extended message not SDTR

sync_msg:
	CLEAR ACK
	MOVE FROM ds_SyncMsg, WHEN MSG_IN
	INT A_int_sdtr			; Let host handle the message
; If we continue from the interrupt, the host has set up a response
; message to be sent.  Set ATN, clear ACK, and continue.
	SET ATN
	CLEAR ACK
	JUMP REL(switch)

disc:
	CLEAR ACK
	WAIT DISCONNECT

	INT A_int_disc_nosdp		; signal disconnect w/o save DP

msg_sdp:
	CLEAR ACK			; acknowledge message
	JUMP REL(switch), WHEN NOT MSG_IN
	MOVE FROM ds_ExtMsg, WHEN MSG_IN
	INT A_int_sdp_nodisc, IF NOT 0x04	; interrupt if not disconnect
	CLEAR ACK
	WAIT DISCONNECT

	INT A_int_disc			; signal disconnect

reselect:
wait_reselect:
	WAIT RESELECT REL(select_adr)
	MOVE LCRC to SFBR		; Save reselect ID
	MOVE SFBR to SCRATCH0

	INT A_int_resel_no_id, WHEN NOT MSG_IN	; didn't get IDENTIFY
	MOVE FROM ds_Msg, WHEN MSG_IN
	INT A_int_resel			; let host know about reconnect
	CLEAR ACK			; acknowledge the message
	JUMP REL(switch)


select_adr:
	MOVE SCNTL1 & 0x10 to SFBR	; get connected status
	INT A_int_sel_fail, IF 0x00	; tell host if not connected
	MOVE CTEST2 & 0x40 to SFBR	; clear Sig_P
	JUMP REL(wait_reselect)		; and try reselect again

msgout:
	MOVE FROM ds_MsgOut, WHEN MSG_OUT
	JUMP REL(switch)

command_phase:
	CLEAR ATN
	MOVE FROM ds_Cmd, WHEN CMD
	JUMP REL(switch)

dataout:
	MOVE FROM ds_Data1, WHEN DATA_OUT
	CALL REL(switch), WHEN NOT DATA_OUT
	MOVE FROM ds_Data2, WHEN DATA_OUT
	CALL REL(switch), WHEN NOT DATA_OUT
	MOVE FROM ds_Data3, WHEN DATA_OUT
	CALL REL(switch), WHEN NOT DATA_OUT
	MOVE FROM ds_Data4, WHEN DATA_OUT
	CALL REL(switch), WHEN NOT DATA_OUT
	MOVE FROM ds_Data5, WHEN DATA_OUT
	CALL REL(switch), WHEN NOT DATA_OUT
	MOVE FROM ds_Data6, WHEN DATA_OUT
	CALL REL(switch), WHEN NOT DATA_OUT
	MOVE FROM ds_Data7, WHEN DATA_OUT
	CALL REL(switch), WHEN NOT DATA_OUT
	MOVE FROM ds_Data8, WHEN DATA_OUT
	CALL REL(switch), WHEN NOT DATA_OUT
	MOVE FROM ds_Data9, WHEN DATA_OUT
	CALL REL(switch)

datain:
	MOVE FROM ds_Data1, WHEN DATA_IN
	CALL REL(switch), WHEN NOT DATA_IN
	MOVE FROM ds_Data2, WHEN DATA_IN
	CALL REL(switch), WHEN NOT DATA_IN
	MOVE FROM ds_Data3, WHEN DATA_IN
	CALL REL(switch), WHEN NOT DATA_IN
	MOVE FROM ds_Data4, WHEN DATA_IN
	CALL REL(switch), WHEN NOT DATA_IN
	MOVE FROM ds_Data5, WHEN DATA_IN
	CALL REL(switch), WHEN NOT DATA_IN
	MOVE FROM ds_Data6, WHEN DATA_IN
	CALL REL(switch), WHEN NOT DATA_IN
	MOVE FROM ds_Data7, WHEN DATA_IN
	CALL REL(switch), WHEN NOT DATA_IN
	MOVE FROM ds_Data8, WHEN DATA_IN
	CALL REL(switch), WHEN NOT DATA_IN
	MOVE FROM ds_Data9, WHEN DATA_IN
	CALL REL(switch)

end:
	MOVE FROM ds_Status, WHEN STATUS
	INT A_int_status_no_msg, WHEN NOT MSG_IN	; status not followed by msg
	MOVE FROM ds_Msg, WHEN MSG_IN
	CLEAR ACK
	WAIT DISCONNECT
	INT A_ok			; signal completion
	JUMP REL(wait_reselect)
