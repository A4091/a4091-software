#ifdef DEBUG_SCSIMSG
#define USE_SERIAL_OUTPUT
#endif

#include "port.h"
#include "printf.h"
#include <exec/types.h>
#include <exec/io.h>
#include <devices/scsidisk.h>
#include "scsimsg.h"

static UBYTE sense_data[255];

int
dev_scsi_inquiry(struct IOExtTD *tio, uint unit, scsi_inquiry_data_t **inq)
{
	int rc;
	scsi_inquiry_data_t *res;
	scsi_generic_t cmd;
	struct SCSICmd scmd;
	uint lun = unit / 10;

#define SCSIPI_INQUIRY_LENGTH_SCSI2	 36
	res = (scsi_inquiry_data_t *) AllocMem(sizeof (*res), MEMF_PUBLIC|MEMF_CLEAR);
	if (res == NULL) {
		printf("AllocMem ");
		*inq = NULL;
		return (1);
	}

	memset(&cmd, 0, sizeof (cmd));
	cmd.opcode = INQUIRY;
	cmd.bytes[0] = lun << 5;
	//cmd.bytes[1] = 0;  // Page code
	//cmd.bytes[2] = 0;
	cmd.bytes[3] = sizeof (scsi_inquiry_data_t);
	//cmd.bytes[4] = 0;  // Control

	memset(&scmd, 0, sizeof (scmd));
	scmd.scsi_Data = (UWORD *) res;
	scmd.scsi_Length = sizeof (*res);

	scmd.scsi_Command = (UBYTE *) &cmd;
	scmd.scsi_CmdLength = 6;

	scmd.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;

	scmd.scsi_SenseData = sense_data;
	scmd.scsi_SenseLength = sizeof (sense_data);

	tio->iotd_Req.io_Command = HD_SCSICMD;
	tio->iotd_Req.io_Length  = sizeof (scmd);
	tio->iotd_Req.io_Data	= &scmd;

	if ((rc = DoIO((struct IORequest *) tio)) != 0) {
		FreeMem(res, sizeof (*res));
		res = NULL;
	}
	*inq = res;
	return (rc);
}

static int
do_scsidirect_cmd(struct IOExtTD *tio, scsi_generic_t *cmd, uint cmdlen,
               void *res, uint reslen)
{
    struct SCSICmd scmd;
    int rc;

    memset(&scmd, 0, sizeof (scmd));
    scmd.scsi_Data = (UWORD *) res;
    scmd.scsi_Length = reslen;

    scmd.scsi_Command = (UBYTE *) cmd;
    scmd.scsi_CmdLength = cmdlen;  // sizeof (cmd);

    scmd.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;

    scmd.scsi_SenseData = sense_data;
    scmd.scsi_SenseLength = sizeof (sense_data);

    tio->iotd_Req.io_Command = HD_SCSICMD;
    tio->iotd_Req.io_Length  = sizeof (scmd);
    tio->iotd_Req.io_Data    = &scmd;

    rc = DoIO((struct IORequest *) tio);

    return (rc);
}

scsi_read_capacity_10_data_t *
dev_scsi_read_capacity_10(struct IOExtTD *tio, uint unit)
{
    uint lun = unit / 10;
    scsi_generic_t cmd;

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = READ_CAPACITY_10;
    cmd.bytes[0] = lun << 5;

    int reslen = sizeof (scsi_read_capacity_10_data_t);
    void *res = AllocMem(reslen, MEMF_PUBLIC | MEMF_CLEAR);
    if (res) {
	if (do_scsidirect_cmd(tio, &cmd, 10, res, reslen)) {
	    FreeMem(res, reslen);
	    res = NULL;
	}
    }
    return res;
}

