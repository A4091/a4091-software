#ifndef SCSIMSG_H
#define SCSIMSG_H 1

#include <devices/trackdisk.h>

#define SCSI_CMD_TEST_UNIT_READY  0x00
#define SCSI_CMD_REQUEST_SENSE    0x03
#define SCSI_CMD_READ_6           0x08
#define SCSI_CMD_WRITE_6          0x0A
#define SCSI_CMD_INQUIRY          0x12
#define SCSI_CMD_MODE_SELECT_6    0x15
#define SCSI_CMD_MODE_SENSE_6     0x1A
#define SCSI_CMD_READ_CAPACITY_10 0x25
#define SCSI_CMD_READ_10          0x28
#define SCSI_CMD_WRITE_10         0x2A
#define SCSI_CMD_READ_TOC         0x43
#define SCSI_CMD_PLAY_AUDIO_MSF   0x47
#define SCSI_CMD_PLAY_TRACK_INDEX 0x48
#define SCSI_CMD_MODE_SELECT_10   0x55
#define SCSI_CMD_MODE_SENSE_10    0x5A
#define SCSI_CMD_START_STOP_UNIT  0x1B
#define SCSI_CMD_ATA_PASSTHROUGH  0xA1
#define SCSI_CHECK_CONDITION      0x02

#define SZ_CDB_10 10
#define SZ_CDB_12 12

#define SCSI_CD_MAX_TRACKS 100

#define SCSI_TOC_SIZE (SCSI_CD_MAX_TRACKS * 8) + 4 // SCSI_CD_MAX_TRACKS track descriptors + the toc header

#define INQUIRY                 SCSI_CMD_INQUIRY

// TODO deduplicate with scsipi_inquiry_data?
typedef struct scsi_inquiry_data {
    uint8_t device;
#define SID_TYPE                0x1f    /* device type mask */
#define SID_QUAL                0xe0    /* device qualifier mask */
#define SID_QUAL_LU_NOTPRESENT  0x20    /* logical unit not present */
    uint8_t dev_qual2;
    uint8_t version;
    uint8_t response_format;
    uint8_t additional_length;
    uint8_t flags1;

    uint8_t flags2;
#define SID_REMOVABLE           0x80
    uint8_t flags3;
#define SID_SftRe       0x01
#define SID_CmdQue      0x02
#define SID_Linked      0x08
#define SID_Sync        0x10
#define SID_WBus16      0x20
#define SID_WBus32      0x40
#define SID_RelAdr      0x80

#define SID_REMOVABLE           0x80
    uint8_t vendor[8];
    uint8_t product[16];
    uint8_t revision[4];
    uint8_t vendor_specific[20];
    uint8_t flags4;
    uint8_t reserved;
    uint8_t version_descriptor[8][2];
} __packed  __attribute__((aligned(2))) scsi_inquiry_data_t;  // 74 bytes

typedef struct scsi_generic {
    uint8_t opcode;
    uint8_t bytes[15];
} __packed scsi_generic_t;

struct __packed SCSI_TOC_TRACK_DESCRIPTOR {
    uint8_t reserved1;
    uint8_t adrControl;
    uint8_t trackNumber;
    uint8_t reserved2;
    uint8_t reserved3;
    uint8_t minute;
    uint8_t second;
    uint8_t frame;
};

struct __packed SCSI_CD_TOC {
    uint16_t length;
    uint8_t  firstTrack;
    uint8_t  lastTrack;
    struct SCSI_TOC_TRACK_DESCRIPTOR td[SCSI_CD_MAX_TRACKS];
};

int dev_scsi_inquiry(struct IOExtTD *tio, uint unit, scsi_inquiry_data_t *inq);
int dev_scsi_get_drivegeometry(struct IOExtTD *tio, struct DriveGeometry *geom);

#endif
