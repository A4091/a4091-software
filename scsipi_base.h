#ifndef _SCSIPI_BASE
#define _SCSIPI_BASE

struct scsipi_xfer *scsipi_get_xs(struct scsipi_periph *, int);
void	scsipi_put_xs(struct scsipi_xfer *);

struct scsipi_xfer * scsipi_make_xs_unlocked(struct scsipi_periph *periph,
    struct scsipi_generic *cmd, int cmdlen, u_char *data_addr, int datalen,
    int retries, int timeout, struct buf *bp, int flags);

struct scsipi_xfer * scsipi_make_xs_locked(struct scsipi_periph *periph,
    struct scsipi_generic *cmd, int cmdlen, u_char *data_addr, int datalen,
    int retries, int timeout, struct buf *bp, int flags);

#endif /* _SCSIPI_BASE */
