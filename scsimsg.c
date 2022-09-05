#ifdef DEBUG_SCSIMSG
#define USE_SERIAL_OUTPUT
#endif

#include "port.h"
#include "printf.h"
#include <exec/types.h>
#include <exec/io.h>
#include <devices/scsidisk.h>
#include <stdint.h>
#include <string.h>
#include "cmdhandler.h"
#include "scsipiconf.h"
#include "scsipi_disk.h"
#include "scsimsg.h"

extern struct MsgPort *myPort;

static int blksize;
UBYTE sense_data[255];

uint16_t sdcmd_read_blocks(struct IOStdReq *ioreq, uint8_t *data, uint32_t block, uint32_t len)
{
    ioreq->io_Command = CMD_READ;
    ioreq->io_Actual  = 0;
    ioreq->io_Offset  = block * blksize;
    ioreq->io_Length  = blksize * len;
    ioreq->io_Data    = data;
    ioreq->io_Flags   = 0;
    ioreq->io_Error   = 0;
    ioreq->io_Message.mn_ReplyPort=CreateMsgPort();

    PutMsg(myPort, &ioreq->io_Message);
    WaitPort(ioreq->io_Message.mn_ReplyPort);
    DeleteMsgPort(ioreq->io_Message.mn_ReplyPort);

    return 0;
}

int do_scsi_inquiry(struct IOExtTD *tio, uint lun, scsi_inquiry_data_t **inq)
{
    int rc;
    scsi_inquiry_data_t *res;
    scsi_generic_t cmd;
    struct SCSICmd scmd;

#define SCSIPI_INQUIRY_LENGTH_SCSI2     36
    res = (scsi_inquiry_data_t *) AllocMem(sizeof (*res), MEMF_PUBLIC);
    if (res == NULL) {
        printf("AllocMem ");
        *inq = NULL;
        return (1);
    }

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = INQUIRY;
    cmd.bytes[0] = lun << 5;
    cmd.bytes[1] = 0;  // Page code
    cmd.bytes[2] = 0;
    cmd.bytes[3] = sizeof (scsi_inquiry_data_t);
    cmd.bytes[4] = 0;  // Control

    memset(&scmd, 0, sizeof (scmd));
    scmd.scsi_Data = (UWORD *) res;
    scmd.scsi_Length = sizeof (*res);
    // scmd.scsi_Actual = 0;
    scmd.scsi_Command = (UBYTE *) &cmd;
    scmd.scsi_CmdLength = 6;
    // scmd.scsi_CmdActual = 0;
    scmd.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
    // scmd.scsi_Status = 0;
    scmd.scsi_SenseData = sense_data;
    scmd.scsi_SenseLength = sizeof (sense_data);
    // scmd.scsi_SenseActual = 0;

    tio->iotd_Req.io_Command = HD_SCSICMD;
    tio->iotd_Req.io_Length  = sizeof (scmd);
    tio->iotd_Req.io_Data    = &scmd;

    tio->iotd_Req.io_Message.mn_ReplyPort=CreateMsgPort();

    PutMsg(myPort, &tio->iotd_Req.io_Message);
    WaitPort(tio->iotd_Req.io_Message.mn_ReplyPort);
    DeleteMsgPort(tio->iotd_Req.io_Message.mn_ReplyPort);

    rc = 0; // FIXME how do I get rc?

    if (rc != 0) {
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
    // scmd.scsi_Actual = 0;
    scmd.scsi_Command = (UBYTE *) cmd;
    scmd.scsi_CmdLength = cmdlen;  // sizeof (cmd);
    // scmd.scsi_CmdActual = 0;
    scmd.scsi_Flags = SCSIF_READ | SCSIF_AUTOSENSE;
    // scmd.scsi_Status = 0;
    scmd.scsi_SenseData = sense_data;
    scmd.scsi_SenseLength = sizeof (sense_data);
    // scmd.scsi_SenseActual = 0;

    tio->iotd_Req.io_Command = HD_SCSICMD;
    tio->iotd_Req.io_Length  = sizeof (scmd);
    tio->iotd_Req.io_Data    = &scmd;

    tio->iotd_Req.io_Message.mn_ReplyPort=CreateMsgPort();

    PutMsg(myPort, &tio->iotd_Req.io_Message);
    WaitPort(tio->iotd_Req.io_Message.mn_ReplyPort);
    DeleteMsgPort(tio->iotd_Req.io_Message.mn_ReplyPort);

    rc = 0; // FIXME how do I get rc?

    return (rc);
}

static void *
do_scsidirect_alloc(struct IOExtTD *tio, scsi_generic_t *cmd, uint cmdlen,
                    uint reslen)
{
    void *res = AllocMem(reslen, MEMF_PUBLIC | MEMF_CLEAR);
    if (res == NULL) {
        printf("AllocMem ");
    } else {
	printf("do_scsidirect_alloc\n");
        if (do_scsidirect_cmd(tio, cmd, cmdlen, res, reslen)) {
            FreeMem(res, reslen);
            res = NULL;
        }
    }
    printf("do_scsidirect_alloc %p\n", res);
    return (res);
}

scsi_read_capacity_10_data_t *
do_scsi_read_capacity_10(struct IOExtTD *tio, uint lun)
{
    scsi_generic_t cmd;

    memset(&cmd, 0, sizeof (cmd));
    cmd.opcode = READ_CAPACITY_10;
    cmd.bytes[0] = lun << 5;

    return (do_scsidirect_alloc(tio, &cmd, 10,
                                sizeof (scsi_read_capacity_10_data_t)));
}

int safe_open(struct IOStdReq *ioreq, uint scsi_unit)
{
    ioreq->io_Message.mn_Node.ln_Type = NT_REPLYMSG;

    if (open_unit(scsi_unit, (void **) &ioreq->io_Unit)) {
        printf("No unit at %d.%d\n", scsi_unit % 10, scsi_unit / 10);
        ioreq->io_Error = HFERR_SelTimeout;
        // HFERR_SelfUnit - attempted to open our own SCSI ID
        return 1;
    }
    int blkshift = ((struct scsipi_periph *) ioreq->io_Unit)->periph_blkshift;
    blksize = (1 << blkshift);
    ioreq->io_Error = 0; // Success

    return 0;
}

void safe_close(struct IOStdReq *ioreq)
{
    close_unit((void *) ioreq->io_Unit);
}


