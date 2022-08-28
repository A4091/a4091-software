#include "port.h"
#include "printf.h"
#include <exec/types.h>
#include <exec/io.h>
#include <devices/scsidisk.h>
#include <stdint.h>
#include <string.h>
#include "cmdhandler.h"
#include "scsipiconf.h"
#include "scsimsg.h"

extern struct MsgPort *myPort;
#define DEFAULT_BLOCK_SIZE 512

static int blksize = DEFAULT_BLOCK_SIZE;
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


