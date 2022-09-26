/*	$NetBSD: scsipi_base.c,v 1.187 2020/09/17 01:19:41 jakllsch Exp $	*/

/*-
 * Copyright (c) 1998, 1999, 2000, 2002, 2003, 2004 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Charles M. Hannum; by Jason R. Thorpe of the Numerical Aerospace
 * Simulation Facility, NASA Ames Research Center.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#ifdef DEBUG_SCSIPI_BASE
#define USE_SERIAL_OUTPUT
#endif

#include "port.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <inline/exec.h>

#if 0
__KERNEL_RCSID(0, "$NetBSD: scsipi_base.c,v 1.187 2020/09/17 01:19:41 jakllsch Exp $");
#endif

#include "scsipiconf.h"
#include "scsi_disk.h"
#include "scsipi_disk.h"
#include "scsipi_base.h"
#include "scsipi_all.h"
#include "scsi_all.h"
#include "scsi_message.h"
#include "sd.h"

#undef SCSIPI_DEBUG
#undef QUEUE_DEBUG


#define ERESTART 100  // Needs restart

static void scsipi_run_queue(struct scsipi_channel *chan);
static int scsipi_complete(struct scsipi_xfer *xs);
static void scsipi_request_sense(struct scsipi_xfer *xs);
void scsipi_completion_poll(struct scsipi_channel *chan);
static void scsipi_put_tag(struct scsipi_xfer *xs);

extern struct ExecBase *SysBase;

#if 0
/*
 * scsipi_update_timeouts:
 * 	Override timeout value if device/config provided
 *      timeouts are available.
 */
static void
scsipi_update_timeouts(struct scsipi_xfer *xs)
{
	struct scsipi_opcodes *opcs;
	u_int8_t cmd;
	int timeout;
	struct scsipi_opinfo *oi;

	if (xs->timeout <= 0) {
		return;
	}

	opcs = xs->xs_periph->periph_opcs;

	if (opcs == NULL) {
		return;
	}

	cmd = xs->cmd->opcode;
	oi = &opcs->opcode_info[cmd];

	timeout = 1000 * (int)oi->ti_timeout;


	if (timeout > xs->timeout && timeout < 86400000) {
		/*
		 * pick up device configured timeouts if they
		 * are longer than the requested ones but less
		 * than a day
		 */
#ifdef SCSIPI_DEBUG
		if ((oi->ti_flags & SCSIPI_TI_LOGGED) == 0) {
			SC_DEBUG(xs->xs_periph, SCSIPI_DB3,
				 ("Overriding command 0x%02x "
				  "timeout of %d with %d ms\n",
				  cmd, xs->timeout, timeout));
			oi->ti_flags |= SCSIPI_TI_LOGGED;
		}
#endif
		xs->timeout = timeout;
	}
}
#endif

#ifdef QUEUE_DEBUG
static void
print_xs_queue(struct scsipi_channel *chan)
{
    struct scsipi_xfer *lxs = NULL;
    struct scsipi_xfer *xs;
    int count = 9;

    printf("queue:");
    for (xs = TAILQ_FIRST(&chan->chan_queue); xs != NULL;
         xs = TAILQ_NEXT(xs, channel_q)) {
        if (xs == lxs) {
            break;
        }
        printf(" %p", xs);
        if (count++ > 10)
            break;
    }
    printf("\n");
}
#endif


/*
 * scsipi_enqueue:
 *
 *	Enqueue an xfer on a channel.
 */
static int
scsipi_enqueue(struct scsipi_xfer *xs)
{
	struct scsipi_channel *chan = xs->xs_periph->periph_channel;
	struct scsipi_xfer *qxs;

	SDT_PROBE1(scsi, base, xfer, enqueue,  xs);

	/*
	 * If the xfer is to be polled, and there are already jobs on
	 * the queue, we can't proceed.
	 */
	KASSERT(mutex_owned(chan_mtx(chan)));
	if ((xs->xs_control & XS_CTL_POLL) != 0 &&
	    TAILQ_FIRST(&chan->chan_queue) != NULL) {
		xs->error = XS_DRIVER_STUFFUP;
		return EAGAIN;
	}

	/*
	 * If we have an URGENT xfer, it's an error recovery command
	 * and it should just go on the head of the channel's queue.
	 */
	if (xs->xs_control & XS_CTL_URGENT) {
#ifdef QUEUE_DEBUG
                printf("[adding %p head]", xs);
                print_xs_queue(chan);
#endif
		TAILQ_INSERT_HEAD(&chan->chan_queue, xs, channel_q);
#ifdef QUEUE_DEBUG
                print_xs_queue(chan);
#endif
		goto out;
	}

	/*
	 * If this xfer has already been on the queue before, we
	 * need to reinsert it in the correct order.  That order is:
	 *
	 *	Immediately before the first xfer for this periph
	 *	with a requeuecnt less than xs->xs_requeuecnt.
	 *
	 * Failing that, at the end of the queue.  (We'll end up
	 * there naturally.)
	 */
	if (xs->xs_requeuecnt != 0) {
		for (qxs = TAILQ_FIRST(&chan->chan_queue); qxs != NULL;
		     qxs = TAILQ_NEXT(qxs, channel_q)) {
			if (qxs->xs_periph == xs->xs_periph &&
			    qxs->xs_requeuecnt < xs->xs_requeuecnt)
				break;
		}
		if (qxs != NULL) {
#ifdef QUEUE_DEBUG
                        printf("[adding %p after %p]", xs, qxs);
                        print_xs_queue(chan);
#endif
			TAILQ_INSERT_AFTER(&chan->chan_queue, qxs, xs,
			    channel_q);
#ifdef QUEUE_DEBUG
                        print_xs_queue(chan);
#endif
			goto out;
		}
	}
#ifdef QUEUE_DEBUG
        printf("[adding %p tail]", xs);
        print_xs_queue(chan);
#endif
	TAILQ_INSERT_TAIL(&chan->chan_queue, xs, channel_q);
#ifdef QUEUE_DEBUG
        print_xs_queue(chan);
#endif
 out:
#if 0
	if (xs->xs_control & XS_CTL_THAW_PERIPH)
		scsipi_periph_thaw_locked(xs->xs_periph, 1);
#endif
	return 0;
}

struct scsipi_xfer *
scsipi_get_xs(struct scsipi_periph *periph, int flags)
{
    /* pool get */
    struct scsipi_xfer *xs;
    struct scsipi_channel *chan = periph->periph_channel;

    xs = chan->chan_xs_free;
    if (xs != NULL) {
        chan->chan_xs_free = *(struct scsipi_xfer **) xs;  /* ->next link */
        memset(xs, 0, sizeof (*xs));
    } else {
        xs = AllocMem(sizeof (*xs), MEMF_CLEAR | MEMF_PUBLIC);
        if (xs == NULL)
            return (xs);
    }

    callout_init(&xs->xs_callout, 0);
    xs->xs_periph = periph;
    xs->xs_control = flags;
    xs->xs_status = 0;
    xs->xs_done_callback = NULL;
    xs->xs_callback_arg = NULL;
    xs->amiga_ior = NULL;

#ifndef PORT_AMIGA
    TAILQ_INSERT_TAIL(&periph->periph_xferq, xs, device_q);

    periph->periph_active++;
#endif
    chan->chan_active++;

#if 0
    printf("get_xs(%p) active=%u\n", xs, chan->chan_active);
#endif
    return (xs);
}

void
scsipi_put_xs(struct scsipi_xfer *xs)
{
    /* pool put */
    struct scsipi_periph  *periph = xs->xs_periph;
    struct scsipi_channel *chan = periph->periph_channel;

#ifndef PORT_AMIGA
    TAILQ_REMOVE(&periph->periph_xferq, xs, device_q);
#endif

#ifndef PORT_AMIGA
    periph->periph_active--;
#endif
    chan->chan_active--;

    /*
     * Insert this entry at the top of the free list. It's really just
     * a very simple linked list, where the first bytes of a given xs
     * point to the next entry. Since the xs is "dead" in the free list,
     * we ignore the fact there are other fields of the struct scsipi_xfer
     * being overwritten by the linked list "next" entry pointer.
     */
    *(struct scsipi_xfer **) xs = chan->chan_xs_free;
    chan->chan_xs_free = xs;

#if 0
    printf("put_xs(%p) active=%u\n", xs, chan->chan_active);
#endif
}

void
scsipi_free_all_xs(struct scsipi_channel *chan)
{
//    struct scsipi_periph *periph;
    struct scsipi_xfer   *xs = chan->chan_xs_free;

    chan->chan_xs_free = NULL;
    while (xs != NULL) {
        struct scsipi_xfer *txs = xs;
        xs = *(struct scsipi_xfer **) xs;  /* ->next link */
        FreeMem(txs, sizeof (*txs));
    }
    chan->chan_active = 0;
}

/*
 * scsipi_execute_xs:
 *
 *	Begin execution of an xfer, waiting for it to complete, if necessary.
 */
int
scsipi_execute_xs(struct scsipi_xfer *xs)
{
	struct scsipi_periph *periph = xs->xs_periph;
	struct scsipi_channel *chan = periph->periph_channel;
	int oasync, async, poll, error;

	KASSERT(!cold);

#ifdef PORT_AMIGA
        /*
         * Set the LUN in the CDB if we have an older device. We also
         * set it for more modern SCSI-2 devices "just in case".
         *
         * CDH: This code is taken from the scsi_base bustype_cmd
         *      implementation, and is placed here to avoid a call
         *      through indirection.
         */
        if (periph->periph_version <= 2)
                xs->cmd->bytes[0] |=
                    ((periph->periph_lun << SCSI_CMD_LUN_SHIFT) &
                        SCSI_CMD_LUN_MASK);
#else
	scsipi_update_timeouts(xs);

	(chan->chan_bustype->bustype_cmd)(xs);
#endif

	xs->xs_status &= ~XS_STS_DONE;
	xs->error = XS_NOERROR;
	xs->resid = xs->datalen;
	xs->status = SCSI_OK;
	SDT_PROBE1(scsi, base, xfer, execute,  xs);

#ifdef SCSIPI_DEBUG
	if (xs->xs_periph->periph_dbflags & SCSIPI_DB3) {
		printf("scsipi_execute_xs: ");
		show_scsipi_xs(xs);
		printf("\n");
	}
#endif

	/*
	 * Deal with command tagging:
	 *
	 *	- If the device's current operating mode doesn't
	 *	  include tagged queueing, clear the tag mask.
	 *
	 *	- If the device's current operating mode *does*
	 *	  include tagged queueing, set the tag_type in
	 *	  the xfer to the appropriate byte for the tag
	 *	  message.
	 */
	if ((PERIPH_XFER_MODE(periph) & PERIPH_CAP_TQING) == 0 ||
		(xs->xs_control & XS_CTL_REQSENSE)) {
		xs->xs_control &= ~XS_CTL_TAGMASK;
		xs->xs_tag_type = 0;
	} else {
		/*
		 * If the request doesn't specify a tag, give Head
		 * tags to URGENT operations and Simple tags to
		 * everything else.
		 */
		if (XS_CTL_TAGTYPE(xs) == 0) {
			if (xs->xs_control & XS_CTL_URGENT)
				xs->xs_control |= XS_CTL_HEAD_TAG;
			else
				xs->xs_control |= XS_CTL_SIMPLE_TAG;
		}

		switch (XS_CTL_TAGTYPE(xs)) {
		case XS_CTL_ORDERED_TAG:
			xs->xs_tag_type = MSG_ORDERED_Q_TAG;
			break;

		case XS_CTL_SIMPLE_TAG:
			xs->xs_tag_type = MSG_SIMPLE_Q_TAG;
			break;

		case XS_CTL_HEAD_TAG:
			xs->xs_tag_type = MSG_HEAD_OF_Q_TAG;
			break;

		default:
			scsipi_printaddr(periph);
			printf("invalid tag mask 0x%08x\n",
			    XS_CTL_TAGTYPE(xs));
#ifdef PORT_AMIGA
			panic("invalid tag mask %x", XS_CTL_TAGTYPE(xs));
#else
			panic("scsipi_execute_xs");
#endif
		}
	}

	/* If the adapter wants us to poll, poll. */
	if (chan->chan_adapter->adapt_flags & SCSIPI_ADAPT_POLL_ONLY)
		xs->xs_control |= XS_CTL_POLL;

	/*
	 * If we don't yet have a completion thread, or we are to poll for
	 * completion, clear the ASYNC flag.
	 */
	oasync = (xs->xs_control & XS_CTL_ASYNC);
	poll = (xs->xs_control & XS_CTL_POLL);
	if (poll != 0)
		xs->xs_control &= ~XS_CTL_ASYNC;
	async = (xs->xs_control & XS_CTL_ASYNC);

#ifdef DIAGNOSTIC
	if (oasync != 0 && xs->bp == NULL)
		panic("scsipi_execute_xs: XS_CTL_ASYNC but no buf");
#endif

	/*
	 * Enqueue the transfer.  If we're not polling for completion, this
	 * should ALWAYS return `no error'.
	 */
	error = scsipi_enqueue(xs);
	if (error) {
		if (poll == 0) {
			scsipi_printaddr(periph);
			printf("not polling, but enqueue failed with %d\n",
			    error);
#ifdef PORT_AMIGA
			panic("not polling but enqueue failed %d", error);
#else
			panic("scsipi_execute_xs");
#endif
		}

		scsipi_printaddr(periph);
		printf("should have flushed queue?\n");
		goto free_xs;
	}

	mutex_exit(chan_mtx(chan));
 restarted:
	scsipi_run_queue(chan);
	mutex_enter(chan_mtx(chan));

	/*
	 * The xfer is enqueued, and possibly running.  If it's to be
	 * completed asynchronously, just return now.
	 */
	if (async)
		return 0;

	/*
	 * Not an asynchronous command; wait for it to complete.
	 */
	while ((xs->xs_status & XS_STS_DONE) == 0) {
		if (poll) {
			scsipi_printaddr(periph);
#ifndef PORT_AMIGA
			printf("polling command not done\n");
			panic("scsipi_execute_xs");
#endif
		}
#ifdef PORT_AMIGA
                irq_and_timer_handler();  // Run timer and interrupts
#endif
		cv_wait(xs_cv(xs), chan_mtx(chan));
	}

	/*
	 * Command is complete.  scsipi_done() has awakened us to perform
	 * the error handling.
	 */
	mutex_exit(chan_mtx(chan));
	error = scsipi_complete(xs);
	if (error == ERESTART) {
                printf("CDH: XS restarted\n");
		goto restarted;
        }

	/*
	 * If it was meant to run async and we cleared async ourselves,
	 * don't return an error here. It has already been handled
	 */
	if (oasync)
		error = 0;
	/*
	 * Command completed successfully or fatal error occurred.  Fall
	 * into....
	 */
	mutex_enter(chan_mtx(chan));
 free_xs:
        if (xs->xs_done_callback != NULL) {
            printf("BUG: xs_done_callback not called\n");
        }
	scsipi_put_xs(xs);
	mutex_exit(chan_mtx(chan));

	/*
	 * Kick the queue, keep it running in case it stopped for some
	 * reason.
	 */
	scsipi_run_queue(chan);
        /*
         * XXX: This is where FS-UAE falls over due to a detected spin loop
         *      in the scripts processor:
         *          lsi_execute_script()
         *              insn_processed > 10000
         *      Workaround in FS-UAE code is to set s->current = NULL and
         *      let it go ahead and force an unexpected device disconnect.
         */

	mutex_enter(chan_mtx(chan));
	return error;
}

/*
 * scsipi_channel_init:
 *
 *      Initialize a scsipi_channel when it is attached.
 */
int
scsipi_channel_init(struct scsipi_channel *chan)
{
#ifndef PORT_AMIGA
        struct scsipi_adapter *adapt = chan->chan_adapter;
#endif
        int i;

#ifndef PORT_AMIGA
        /* Initialize shared data. */
        scsipi_init();
#endif
        chan->chan_xs_free = NULL;

        /* Initialize the queues. */
        TAILQ_INIT(&chan->chan_queue);
        TAILQ_INIT(&chan->chan_complete);

        for (i = 0; i < SCSIPI_CHAN_PERIPH_BUCKETS; i++)
                LIST_INIT(&chan->chan_periphtab[i]);

#ifndef PORT_AMIGA
        /*
         * Create the asynchronous completion thread.
         */
        if (kthread_create(PRI_NONE, 0, NULL, scsipi_completion_thread, chan,
            &chan->chan_thread, "%s", chan->chan_name)) {
                aprint_error_dev(adapt->adapt_dev, "unable to create completion thread for "
                    "channel %d\n", chan->chan_channel);
                panic("scsipi_channel_init");
        }
#endif

        return 0;
}

uint32_t
scsipi_chan_periph_hash(uint64_t t, uint64_t l)
{
        return (0);
}

/*
 * scsipi_insert_periph:
 *
 *	Insert a periph into the channel.
 */
void
scsipi_insert_periph(struct scsipi_channel *chan, struct scsipi_periph *periph)
{
	uint32_t hash;

	hash = scsipi_chan_periph_hash(periph->periph_target,
	    periph->periph_lun);

	mutex_enter(chan_mtx(chan));
	LIST_INSERT_HEAD(&chan->chan_periphtab[hash], periph, periph_hash);
	mutex_exit(chan_mtx(chan));
}

/*
 * scsipi_remove_periph:
 *
 *	Remove a periph from the channel.
 */
void
scsipi_remove_periph(struct scsipi_channel *chan,
    struct scsipi_periph *periph)
{

	LIST_REMOVE(periph, periph_hash);
}

/*
 * scsipi_lookup_periph:
 *
 *	Lookup a periph on the specified channel.
 */
static struct scsipi_periph *
scsipi_lookup_periph_internal(struct scsipi_channel *chan, int target, int lun, bool lock)
{
	struct scsipi_periph *periph;
	uint32_t hash;

#ifndef PORT_AMIGA
	if (target >= chan->chan_ntargets ||
	    lun >= chan->chan_nluns)
		return NULL;
#endif

	hash = scsipi_chan_periph_hash(target, lun);

	if (lock)
		mutex_enter(chan_mtx(chan));
	LIST_FOREACH(periph, &chan->chan_periphtab[hash], periph_hash) {
		if (periph->periph_target == target &&
		    periph->periph_lun == lun)
			break;
	}
	if (lock)
		mutex_exit(chan_mtx(chan));

	return periph;
}

struct scsipi_periph *
scsipi_lookup_periph(struct scsipi_channel *chan, int target, int lun)
{
	return scsipi_lookup_periph_internal(chan, target, lun, true);
}


struct scsipi_xfer *
scsipi_make_xs_internal(struct scsipi_periph *periph,
                        struct scsipi_generic *cmd,
                        int cmdlen, u_char *data_addr, int datalen,
                        int retries, int timeout, struct buf *bp, int flags)
{
    struct scsipi_xfer *xs;

    if ((xs = scsipi_get_xs(periph, flags)) == NULL)
        return (NULL);

    /*
     * Fill out the scsipi_xfer structure.  We don't know whose context
     * the cmd is in, so copy it.
     */
#ifdef PORT_AMIGA
    CopyMem(cmd, &xs->cmdstore, cmdlen);
#else
    memcpy(&xs->cmdstore, cmd, cmdlen);
#endif
    xs->cmd = &xs->cmdstore;
    xs->cmdlen = cmdlen;
    xs->data = data_addr;
    xs->datalen = datalen;
    xs->xs_retries = retries;
    xs->timeout = timeout;
    xs->bp = bp;

    if (timeout == 0)
        printf("WARNING: xs new timeout is ZERO for cmd %x\n", cmd->opcode);
    return (xs);
}

struct scsipi_xfer *
scsipi_make_xs_locked(struct scsipi_periph *periph, struct scsipi_generic *cmd,
    int cmdlen, u_char *data_addr, int datalen, int retries, int timeout,
    struct buf *bp, int flags)
{
    return (scsipi_make_xs_internal(periph, cmd, cmdlen, data_addr,
                                    datalen, retries, timeout, bp,
                                    flags | XS_CTL_NOSLEEP));
}

/*
 * scsipi_get_resource:
 *
 *	Allocate a single xfer `resource' from the channel.
 *
 *	NOTE: Must be called with channel lock held
 */
static int
scsipi_get_resource(struct scsipi_channel *chan)
{
	struct scsipi_adapter *adapt = chan->chan_adapter;

	if (chan->chan_flags & SCSIPI_CHAN_OPENINGS) {
		if (chan->chan_openings > 0) {
			chan->chan_openings--;
			return 1;
		}
		return 0;
	}

	if (adapt->adapt_openings > 0) {
		adapt->adapt_openings--;
		return 1;
	}
	return 0;
}

void
scsipi_adapter_request(struct scsipi_channel *chan,
        scsipi_adapter_req_t req, void *arg)

{
    struct scsipi_adapter *adapt = chan->chan_adapter;

//  scsipi_adapter_lock(adapt);
    SDT_PROBE3(scsi, base, adapter, request__start,  chan, req, arg);
    (adapt->adapt_request)(chan, req, arg);
    SDT_PROBE3(scsi, base, adapter, request__done,  chan, req, arg);
//  scsipi_adapter_unlock(adapt);
}

/*
 * scsipi_grow_resources:
 *
 *	Attempt to grow resources for a channel.  If this succeeds,
 *	we allocate one for our caller.
 *
 *	NOTE: Must be called with channel lock held
 */
static inline int
scsipi_grow_resources(struct scsipi_channel *chan)
{

	if (chan->chan_flags & SCSIPI_CHAN_CANGROW) {
#ifndef PORT_AMIGA
		if ((chan->chan_flags & SCSIPI_CHAN_TACTIVE) == 0) {
#endif
                        /*
                         * Amiga driver doesn't wait for completion thread
                         * to grow resources.
                         */
			mutex_exit(chan_mtx(chan));
			scsipi_adapter_request(chan,
			    ADAPTER_REQ_GROW_RESOURCES, NULL);
			mutex_enter(chan_mtx(chan));
			return scsipi_get_resource(chan);
#ifndef PORT_AMIGA
		}
		/*
		 * ask the channel thread to do it. It'll have to thaw the
		 * queue
		 */
		scsipi_channel_freeze_locked(chan, 1);
		chan->chan_tflags |= SCSIPI_CHANT_GROWRES;
//		cv_broadcast(chan_cv_complete(chan));
#endif
		return 0;
	}

	return 0;
}

/*
 * scsipi_put_resource:
 *
 *	Free a single xfer `resource' to the channel.
 *
 *	NOTE: Must be called with channel lock held
 */
static void
scsipi_put_resource(struct scsipi_channel *chan)
{
	struct scsipi_adapter *adapt = chan->chan_adapter;

	if (chan->chan_flags & SCSIPI_CHAN_OPENINGS)
		chan->chan_openings++;
	else
		adapt->adapt_openings++;
}


/*
 * scsipi_get_tag:
 *
 *	Get a tag ID for the specified xfer.
 *
 *	NOTE: Must be called with channel lock held
 */
static void
scsipi_get_tag(struct scsipi_xfer *xs)
{
	struct scsipi_periph *periph = xs->xs_periph;
	int bit, tag;
	u_int word;

	KASSERT(mutex_owned(chan_mtx(periph->periph_channel)));

	bit = 0;	/* XXX gcc */
	for (word = 0; word < PERIPH_NTAGWORDS; word++) {
		bit = ffs(periph->periph_freetags[word]);
		if (bit != 0)
			break;
	}
#ifdef DIAGNOSTIC
	if (word == PERIPH_NTAGWORDS) {
		scsipi_printaddr(periph);
		printf("no free tags\n");
		panic("scsipi_get_tag");
	}
#endif

	bit -= 1;
	periph->periph_freetags[word] &= ~(1U << bit);
	tag = (word << 5) | bit;

	/* XXX Should eventually disallow this completely. */
	if (tag >= periph->periph_openings) {
		scsipi_printaddr(periph);
		printf("WARNING: tag %d greater than available openings %d\n",
		    tag, periph->periph_openings);
	}

	xs->xs_tag_id = tag;
	SDT_PROBE3(scsi, base, tag, get,
	    xs, xs->xs_tag_id, xs->xs_tag_type);
}

/*
 * scsipi_put_tag:
 *
 *	Put the tag ID for the specified xfer back into the pool.
 *
 *	NOTE: Must be called with channel lock held
 */
static void
scsipi_put_tag(struct scsipi_xfer *xs)
{
	struct scsipi_periph *periph = xs->xs_periph;
	int word, bit;

	KASSERT(mutex_owned(chan_mtx(periph->periph_channel)));

	SDT_PROBE3(scsi, base, tag, put,
	    xs, xs->xs_tag_id, xs->xs_tag_type);

	word = xs->xs_tag_id >> 5;
	bit = xs->xs_tag_id & 0x1f;

	periph->periph_freetags[word] |= (1U << bit);
}

/*
 * scsipi_run_queue:
 *
 *	Start as many xfers as possible running on the channel.
 */
static void
scsipi_run_queue(struct scsipi_channel *chan)
{
	struct scsipi_xfer *xs;
	struct scsipi_periph *periph;

	for (;;) {
#ifdef PORT_AMIGA
		/*
		 * A reset is pending on channel - don't issue anything new.
		 */
		if (chan->chan_flags & SCSIPI_CHAN_RESET_PEND)
			break;
#else
		/*
		 * If the channel is frozen, we can't do any work right
		 * now.
		 */
		if (chan->chan_qfreeze != 0) {
			break;
		}
#endif

		/*
		 * Look for work to do, and make sure we can do it.
		 */
		for (xs = TAILQ_FIRST(&chan->chan_queue); xs != NULL;
		     xs = TAILQ_NEXT(xs, channel_q)) {
			periph = xs->xs_periph;

#ifdef PORT_AMIGA
			if ((periph->periph_sent >= periph->periph_openings) ||
			    (periph->periph_flags & PERIPH_UNTAG) != 0) {
                            /*
                             * PERIPH_UNTAG means the device is running a
                             * single untagged command. No other commands
                             * are allowed at this time.
                             */
                                if (xs == TAILQ_LAST(&chan->chan_queue, scsipi_xfer_queue))
                                    break;  // Last entry in queue
				continue;
                        }
#else
			if ((periph->periph_sent >= periph->periph_openings) ||
			    periph->periph_qfreeze != 0 ||
			    (periph->periph_flags & PERIPH_UNTAG) != 0)
				continue;
#endif

			if ((periph->periph_flags &
			    (PERIPH_RECOVERING | PERIPH_SENSE)) != 0 &&
			    (xs->xs_control & XS_CTL_URGENT) == 0)
				continue;

			/*
			 * We can issue this xfer!
			 */
			goto got_one;
		}

		/*
		 * Can't find any work to do right now.
		 */
		break;

 got_one:
		/*
		 * Have an xfer to run.  Allocate a resource from
		 * the adapter to run it.  If we can't allocate that
		 * resource, we don't dequeue the xfer.
		 */
		if (scsipi_get_resource(chan) == 0) {
			/*
			 * Adapter is out of resources.  If the adapter
			 * supports it, attempt to grow them.
			 */
			if (scsipi_grow_resources(chan) == 0) {
				/*
				 * Wasn't able to grow resources,
				 * nothing more we can do.
				 */
				if (xs->xs_control & XS_CTL_POLL) {
					scsipi_printaddr(xs->xs_periph);
					printf("polling command but no "
					    "adapter resources");
					/* We'll panic shortly... */
				}

				/*
				 * XXX: We should be able to note that
				 * XXX: that resources are needed here!
				 */
				break;
			}
			/*
			 * scsipi_grow_resources() allocated the resource
			 * for us.
			 */
		}

		/*
		 * We have a resource to run this xfer, do it!
		 */
#ifdef QUEUE_DEBUG
                printf("[removing %p]\n", xs);
                print_xs_queue(chan);
#endif
		TAILQ_REMOVE(&chan->chan_queue, xs, channel_q);
#ifdef QUEUE_DEBUG
                print_xs_queue(chan);
#endif

		/*
		 * If the command is to be tagged, allocate a tag ID
		 * for it.
		 */
		if (XS_CTL_TAGTYPE(xs) != 0)
			scsipi_get_tag(xs);
		else
			periph->periph_flags |= PERIPH_UNTAG;
		periph->periph_sent++;

		scsipi_adapter_request(chan, ADAPTER_REQ_RUN_XFER, xs);
	}
}

/*
 * scsipi_done:
 *
 *	This routine is called by an adapter's interrupt handler when
 *	an xfer is completed.
 */
void
scsipi_done(struct scsipi_xfer *xs)
{
	struct scsipi_periph *periph = xs->xs_periph;
	struct scsipi_channel *chan = periph->periph_channel;
	int freezecnt;

	SC_DEBUG(periph, SCSIPI_DB2, ("scsipi_done\n"));
#ifdef SCSIPI_DEBUG
	if (periph->periph_dbflags & SCSIPI_DB1)
		show_scsipi_cmd(xs);
#endif

	SDT_PROBE1(scsi, base, xfer, done,  xs);
	/*
	 * The resource this command was using is now free.
	 */
	if (xs->xs_status & XS_STS_DONE) {
		/* XXX in certain circumstances, such as a device
		 * being detached, a xs that has already been
		 * scsipi_done()'d by the main thread will be done'd
		 * again by scsibusdetach(). Putting the xs on the
		 * chan_complete queue causes list corruption and
		 * everyone dies. This prevents that, but perhaps
		 * there should be better coordination somewhere such
		 * that this won't ever happen (and can be turned into
		 * a KASSERT().
		 */
		SDT_PROBE1(scsi, base, xfer, redone,  xs);
		goto out;
	}
	scsipi_put_resource(chan);
	xs->xs_periph->periph_sent--;

	/*
	 * If the command was tagged, free the tag.
	 */
	if (XS_CTL_TAGTYPE(xs) != 0)
		scsipi_put_tag(xs);
	else
		periph->periph_flags &= ~PERIPH_UNTAG;

	/* Mark the command as `done'. */
	xs->xs_status |= XS_STS_DONE;

#ifdef DIAGNOSTIC
	if ((xs->xs_control & (XS_CTL_ASYNC|XS_CTL_POLL)) ==
	    (XS_CTL_ASYNC|XS_CTL_POLL))
		panic("scsipi_done: ASYNC and POLL");
#endif

	/*
	 * If the xfer had an error of any sort, freeze the
	 * periph's queue.  Freeze it again if we were requested
	 * to do so in the xfer.
	 */
	freezecnt = 0;
	if (xs->error != XS_NOERROR)
		freezecnt++;
	if (xs->xs_control & XS_CTL_FREEZE_PERIPH)
		freezecnt++;
#if 0
	if (freezecnt != 0)
		scsipi_periph_freeze_locked(periph, freezecnt);
#endif

	/*
	 * record the xfer with a pending sense, in case a SCSI reset is
	 * received before the thread is waked up.
	 */
	if (xs->error == XS_BUSY && xs->status == SCSI_CHECK) {
		periph->periph_flags |= PERIPH_SENSE;
		periph->periph_xscheck = xs;
	}

	/*
	 * If this was an xfer that was not to complete asynchronously,
	 * let the requesting thread perform error checking/handling
	 * in its context.
	 */
	if ((xs->xs_control & XS_CTL_ASYNC) == 0) {
		/*
		 * If it's a polling job, just return, to unwind the
		 * call graph.  We don't need to restart the queue,
		 * because polling jobs are treated specially, and
		 * are really only used during crash dumps anyway
		 * (XXX or during boot-time autoconfiguration of
		 * ATAPI devices).
		 */
		if (xs->xs_control & XS_CTL_POLL) {
			return;
		}
//		cv_broadcast(xs_cv(xs));
		goto out;
	}

	/*
	 * Catch the extremely common case of I/O completing
	 * without error; no use in taking a context switch
	 * if we can handle it in interrupt context.
	 */
	if (xs->error == XS_NOERROR) {
                (void) scsipi_complete(xs);
		goto out;
	}

	/*
	 * There is an error on this xfer.  Put it on the channel's
	 * completion queue, and wake up the completion thread.
	 */
#ifdef QUEUE_DEBUG
        printf("(adding %p)", xs);
#endif
	TAILQ_INSERT_TAIL(&chan->chan_complete, xs, channel_q);
#ifndef PORT_AMIGA
	cv_broadcast(chan_cv_complete(chan));
#endif

 out:
	/*
	 * If there are more xfers on the channel's queue, attempt to
	 * run them.
	 */
	scsipi_run_queue(chan);
}

/*
 * scsipi_completion_poll
 * ----------------------
 * Finish completions or perform other channel thread service operations.
 */
void
scsipi_completion_poll(struct scsipi_channel *chan)
{
    struct scsipi_xfer *xs;

    chan->chan_flags |= SCSIPI_CHAN_TACTIVE;
    do {
#ifndef PORT_AMIGA
        if (chan->chan_tflags & SCSIPI_CHANT_GROWRES) {
            /* attempt to get more openings for this channel */
            chan->chan_tflags &= ~SCSIPI_CHANT_GROWRES;
            mutex_exit(chan_mtx(chan));
            scsipi_adapter_request(chan,
                ADAPTER_REQ_GROW_RESOURCES, NULL);
            scsipi_channel_thaw(chan, 1);
            if (chan->chan_tflags & SCSIPI_CHANT_GROWRES)
                delay(100000);
            mutex_enter(chan_mtx(chan));
            continue;
        }
#endif
        if (chan->chan_tflags & SCSIPI_CHANT_KICK) {
            /* explicitly run the queues for this channel */
            chan->chan_tflags &= ~SCSIPI_CHANT_KICK;
            mutex_exit(chan_mtx(chan));
            scsipi_run_queue(chan);
            mutex_enter(chan_mtx(chan));
            continue;
        }
        if (chan->chan_tflags & SCSIPI_CHANT_SHUTDOWN)
            break;

        xs = TAILQ_FIRST(&chan->chan_complete);
        if (xs) {
#ifdef QUEUE_DEBUG
            printf("(removing %p)", xs);
#endif
            TAILQ_REMOVE(&chan->chan_complete, xs, channel_q);
            mutex_exit(chan_mtx(chan));

            /*
             * Have an xfer with an error; process it.
             */
            (void) scsipi_complete(xs);

            /*
             * Kick the queue; keep it running if it was stopped
             * for some reason.
             */
            scsipi_run_queue(chan);
            mutex_enter(chan_mtx(chan));
        }
    } while (xs != NULL);
#ifndef PORT_AMIGA  // Amiga driver runs this as part of command handler thread
    chan->chan_flags &= ~SCSIPI_CHAN_TACTIVE;
#endif
}


/*
 * scsipi_complete:
 *
 *	Completion of a scsipi_xfer.  This is the guts of scsipi_done().
 *
 *	NOTE: This routine MUST be called with valid thread context
 *	except for the case where the following two conditions are
 *	true:
 *
 *		xs->error == XS_NOERROR
 *		XS_CTL_ASYNC is set in xs->xs_control
 *
 *	The semantics of this routine can be tricky, so here is an
 *	explanation:
 *
 *		0		Xfer completed successfully.
 *
 *		ERESTART	Xfer had an error, but was restarted.
 *
 *		anything else	Xfer had an error, return value is Unix
 *				errno.
 *
 *	If the return value is anything but ERESTART:
 *
 *		- If XS_CTL_ASYNC is set, `xs' has been freed back to
 *		  the pool.
 *		- If there is a buf associated with the xfer,
 *		  it has been biodone()'d.
 */
static int
scsipi_complete(struct scsipi_xfer *xs)
{
	struct scsipi_periph *periph = xs->xs_periph;
	struct scsipi_channel *chan = periph->periph_channel;
	int error;

	SDT_PROBE1(scsi, base, xfer, complete,  xs);

#ifdef DIAGNOSTIC
	if ((xs->xs_control & XS_CTL_ASYNC) != 0 && xs->bp == NULL)
		panic("scsipi_complete: XS_CTL_ASYNC but no buf");
#endif
	/*
	 * If command terminated with a CHECK CONDITION, we need to issue a
	 * REQUEST_SENSE command. Once the REQUEST_SENSE has been processed
	 * we'll have the real status.
	 * Must be processed with channel lock held to avoid missing
	 * a SCSI bus reset for this command.
	 */
	mutex_enter(chan_mtx(chan));
	if (xs->error == XS_BUSY && xs->status == SCSI_CHECK) {
		/* request sense for a request sense ? */
		if (xs->xs_control & XS_CTL_REQSENSE) {
			scsipi_printaddr(periph);
			printf("request sense for a request sense ?\n");
			/* XXX maybe we should reset the device ? */
			/* we've been frozen because xs->error != XS_NOERROR */
#if 0
			scsipi_periph_thaw_locked(periph, 1);
#endif
			mutex_exit(chan_mtx(chan));
			if (xs->resid < xs->datalen) {
				printf("we read %d bytes of sense anyway:\n",
				    xs->datalen - xs->resid);
//				scsipi_print_sense_data((void *)xs->data, 0);
			}
			return EINVAL;
		}
		mutex_exit(chan_mtx(chan)); // XXX allows other commands to queue or run
		scsipi_request_sense(xs);
#if 0
                printf("Got sense:");
                for (int t = 0; t < sizeof (xs->sense.scsi_sense); t++)
                    printf(" %02x", ((uint8_t *) &xs->sense.scsi_sense)[t]);
                printf("\n");
#endif
	} else
		mutex_exit(chan_mtx(chan));

#if 0
/*
 * cdh disabled for now -- not sure, but might need this if
 * scsidirect is not asking for sense data.
 */
	/*
	 * If it's a user level request, bypass all usual completion
	 * processing, let the user work it out..
	 */
	if ((xs->xs_control & XS_CTL_USERCMD) != 0) {
		SC_DEBUG(periph, SCSIPI_DB3, ("calling user done()\n"));
		mutex_enter(chan_mtx(chan));
#if 0
		if (xs->error != XS_NOERROR)
			scsipi_periph_thaw_locked(periph, 1);
#endif
		mutex_exit(chan_mtx(chan));
		scsipi_user_done(xs);
		SC_DEBUG(periph, SCSIPI_DB3, ("returned from user done()\n "));
		return 0;
	}
#endif

	switch (xs->error) {
	case XS_NOERROR:
		error = 0;
		break;

	case XS_SENSE:
	case XS_SHORTSENSE:
//		error = (*chan->chan_bustype->bustype_interpret_sense)(xs);
                error = scsipi_interpret_sense(xs);
		break;

	case XS_RESOURCE_SHORTAGE:
		/*
		 * XXX Should freeze channel's queue.
		 */
		scsipi_printaddr(periph);
		printf("adapter resource shortage\n");
		/* FALLTHROUGH */

	case XS_BUSY:
#if 0
		if (xs->error == XS_BUSY && xs->status == SCSI_QUEUE_FULL) {
			struct scsipi_max_openings mo;

			/*
			 * We set the openings to active - 1, assuming that
			 * the command that got us here is the first one that
			 * can't fit into the device's queue.  If that's not
			 * the case, I guess we'll find out soon enough.
			 */
			mo.mo_target = periph->periph_target;
			mo.mo_lun = periph->periph_lun;
			if (periph->periph_active < periph->periph_openings)
				mo.mo_openings = periph->periph_active - 1;
			else
				mo.mo_openings = periph->periph_openings - 1;
#ifdef DIAGNOSTIC
			if (mo.mo_openings < 0) {
				scsipi_printaddr(periph);
				printf("QUEUE FULL resulted in < 0 openings\n");
				panic("scsipi_done");
			}
#endif
			if (mo.mo_openings == 0) {
				scsipi_printaddr(periph);
				printf("QUEUE FULL resulted in 0 openings\n");
				mo.mo_openings = 1;
			}
			scsipi_async_event(chan, ASYNC_EVENT_MAX_OPENINGS, &mo);
			error = ERESTART;
		} else if (xs->xs_retries != 0) {
#else
// }
		if (xs->xs_retries != 0) {
#endif
			xs->xs_retries--;
			/*
			 * Wait one second, and try again.
			 */
			mutex_enter(chan_mtx(chan));
			if ((xs->xs_control & XS_CTL_POLL) ||
			    (chan->chan_flags & SCSIPI_CHAN_TACTIVE) == 0) {
#ifdef PORT_AMIGA
				/*
				 * Wait at least 1 second, running timer and
				 * interrupts until we've seen two timer
				 * messages.
				  */
				int count = 0;
				do {
				    count += irq_and_timer_handler();
				} while (count < 2);
#else  /* !PORT_AMIGA */
				/* XXX: quite extreme */
				kpause("xsbusy", false, hz, chan_mtx(chan));
			} else if (!callout_pending(&periph->periph_callout)) {
				scsipi_periph_freeze_locked(periph, 1);
				callout_reset(&periph->periph_callout,
				    hz, scsipi_periph_timed_thaw, periph);
#endif
			}
			mutex_exit(chan_mtx(chan));
			error = ERESTART;
		} else
			error = EBUSY;
		break;

	case XS_REQUEUE:
		error = ERESTART;
		break;

	case XS_SELTIMEOUT:
	case XS_TIMEOUT:
		/*
		 * If the device hasn't gone away, honor retry counts.
		 *
		 * Note that if we're in the middle of probing it,
		 * it won't be found because it isn't here yet so
		 * we won't honor the retry count in that case.
		 */
		if (scsipi_lookup_periph(chan, periph->periph_target,
		    periph->periph_lun) && xs->xs_retries != 0) {
			xs->xs_retries--;
			error = ERESTART;
		} else
			error = EIO;
		break;

	case XS_RESET:
		if (xs->xs_control & XS_CTL_REQSENSE) {
			/*
			 * request sense interrupted by reset: signal it
			 * with EINTR return code.
			 */
			error = EINTR;
		} else {
			if (xs->xs_retries != 0) {
				xs->xs_retries--;
				error = ERESTART;
			} else
				error = EIO;
		}
		break;

	case XS_DRIVER_STUFFUP:
		scsipi_printaddr(periph);
		printf("generic HBA error\n");
		error = EIO;
		break;
	default:
		scsipi_printaddr(periph);
		printf("invalid return code from adapter: %d\n", xs->error);
		error = EIO;
		break;
	}

	mutex_enter(chan_mtx(chan));
	if (error == ERESTART) {
                printf("restart %p retries left=%d\n", xs, xs->xs_retries);
		SDT_PROBE1(scsi, base, xfer, restart,  xs);
		/*
		 * If we get here, the periph has been thawed and frozen
		 * again if we had to issue recovery commands.  Alternatively,
		 * it may have been frozen again and in a timed thaw.  In
		 * any case, we thaw the periph once we re-enqueue the
		 * command.  Once the periph is fully thawed, it will begin
		 * operation again.
		 */
		xs->error = XS_NOERROR;
		xs->status = SCSI_OK;
		xs->xs_status &= ~XS_STS_DONE;
		xs->xs_requeuecnt++;
		error = scsipi_enqueue(xs);
		if (error == 0) {
#if 0
			scsipi_periph_thaw_locked(periph, 1);
#endif
			mutex_exit(chan_mtx(chan));
			return ERESTART;
		}
	}

	/*
	 * scsipi_done() freezes the queue if not XS_NOERROR.
	 * Thaw it here.
	 */
#if 0
	if (xs->error != XS_NOERROR)
		scsipi_periph_thaw_locked(periph, 1);
	mutex_exit(chan_mtx(chan));

	if (periph->periph_switch->psw_done)
		periph->periph_switch->psw_done(xs, error);
#endif

        /*
         * The callback is used by multiple requesters to trigger an
         * asynchronous I/O continuation:
         *
         * sd_complete() geom_done_inquiry() scsidirect_complete()
         * geom_done_get_capacity() geom_done_mode_page_3()
         * geom_done_mode_page_4() geom_done_mode_page_5()
         */
        if (xs->xs_done_callback != NULL)
            xs->xs_done_callback(xs);

	mutex_enter(chan_mtx(chan));
	if (xs->xs_control & XS_CTL_ASYNC)
		scsipi_put_xs(xs);
	mutex_exit(chan_mtx(chan));

	return error;
}

int
scsipi_command(struct scsipi_periph *periph, struct scsipi_generic *cmd,
    int cmdlen, u_char *data_addr, int datalen, int retries, int timeout,
    struct buf *bp, int flags)
{
        struct scsipi_xfer *xs;
        int rc;

        xs = scsipi_make_xs_locked(periph, cmd, cmdlen, data_addr, datalen,
            retries, timeout, bp, flags);
        if (!xs)
                return (ENOMEM);

        mutex_enter(chan_mtx(periph->periph_channel));
        rc = scsipi_execute_xs(xs);
        mutex_exit(chan_mtx(periph->periph_channel));

        return rc;
}

#if 0
/* stubbed routine */
int
scsipi_print_sense(struct scsipi_xfer * xs, int verbosity)
{
        scsipi_load_verbose();
        if (scsi_verbose_loaded)
                return scsipi_print_sense(xs, verbosity);
        else
                return 0;
}
#endif

/*
 * scsipi_interpret_sense:
 *
 *	Look at the returned sense and act on the error, determining
 *	the unix error number to pass back.  (0 = report no error)
 *
 *	NOTE: If we return ERESTART, we are expected to have
 *	thawed the device!
 *
 *	THIS IS THE DEFAULT ERROR HANDLER FOR SCSI DEVICES.
 */
int
scsipi_interpret_sense(struct scsipi_xfer *xs)
{
	struct scsi_sense_data *sense;
	struct scsipi_periph *periph = xs->xs_periph;
	u_int8_t key;
	int error;
#ifdef USE_SERIAL_OUTPUT
	u_int32_t info;
	static const char *error_mes[] = {
		"soft error (corrected)",
		"not ready", "medium error",
		"non-media hardware failure", "illegal request",
		"unit attention", "readonly device",
		"no data found", "vendor unique",
		"copy aborted", "command aborted",
		"search returned equal", "volume overflow",
		"verify miscompare", "unknown error key"
	};
#endif

	sense = &xs->sense.scsi_sense;
#ifdef SCSIPI_DEBUG
	if (periph->periph_flags & SCSIPI_DB1) {
	        int count, len;
		scsipi_printaddr(periph);
		printf(" sense debug information:\n");
		printf("\tcode 0x%x valid %d\n",
			SSD_RCODE(sense->response_code),
			sense->response_code & SSD_RCODE_VALID ? 1 : 0);
		printf("\tseg 0x%x key 0x%x ili 0x%x eom 0x%x fmark 0x%x\n",
			sense->segment,
			SSD_SENSE_KEY(sense->flags),
			sense->flags & SSD_ILI ? 1 : 0,
			sense->flags & SSD_EOM ? 1 : 0,
			sense->flags & SSD_FILEMARK ? 1 : 0);
		printf("info: 0x%x 0x%x 0x%x 0x%x followed by %d "
			"extra bytes\n",
			sense->info[0],
			sense->info[1],
			sense->info[2],
			sense->info[3],
			sense->extra_len);
		len = SSD_ADD_BYTES_LIM(sense);
		printf("\textra (up to %d bytes): ", len);
		for (count = 0; count < len; count++)
			printf("0x%x ", sense->csi[count]);
		printf("\n");
	}
#endif

#if 0
	/*
	 * If the periph has its own error handler, call it first.
	 * If it returns a legit error value, return that, otherwise
	 * it wants us to continue with normal error processing.
	 */
	if (periph->periph_switch->psw_error != NULL) {
		SC_DEBUG(periph, SCSIPI_DB2,
		    ("calling private err_handler()\n"));
		error = (*periph->periph_switch->psw_error)(xs);
		if (error != EJUSTRETURN)
			return error;
	}
#endif
	/* otherwise use the default */
	switch (SSD_RCODE(sense->response_code)) {

		/*
		 * Old SCSI-1 and SASI devices respond with
		 * codes other than 70.
		 */
	case 0x00:		/* no error (command completed OK) */
		return 0;
	case 0x04:		/* drive not ready after it was selected */
#ifdef PORT_AMIGA
		if ((periph->periph_flags & PERIPH_REMOVABLE) != 0)
			sd_media_unloaded(periph);
#else
		if ((periph->periph_flags & PERIPH_REMOVABLE) != 0)
			periph->periph_flags &= ~PERIPH_MEDIA_LOADED;
#endif
		if ((xs->xs_control & XS_CTL_IGNORE_NOT_READY) != 0)
			return 0;
		/* XXX - display some sort of error here? */
#ifdef PORT_AMIGA
                printf("Drive not ready after select: asc=%02x ascq=%02x\n",
                       sense->asc, sense->ascq);
#else
                printf("drive not ready after select\n");
#endif
		return EIO;
	case 0x20:		/* invalid command */
		if ((xs->xs_control &
		     XS_CTL_IGNORE_ILLEGAL_REQUEST) != 0)
			return 0;
		return EINVAL;
	case 0x25:		/* invalid LUN (Adaptec ACB-4000) */
		return EACCES;

		/*
		 * If it's code 70, use the extended stuff and
		 * interpret the key
		 */
	case 0x71:		/* delayed error */
		scsipi_printaddr(periph);
		key = SSD_SENSE_KEY(sense->flags);
		printf(" DEFERRED ERROR, key = 0x%x\n", key);
		/* FALLTHROUGH */
	case 0x70:
#ifdef USE_SERIAL_OUTPUT
		if ((sense->response_code & SSD_RCODE_VALID) != 0)
			info = _4btol(sense->info);
		else
			info = 0;
#endif
		key = SSD_SENSE_KEY(sense->flags);

		switch (key) {
		case SKEY_NO_SENSE:
		case SKEY_RECOVERED_ERROR:
			if (xs->resid == xs->datalen && xs->datalen) {
				/*
				 * Why is this here?
				 */
				xs->resid = 0;	/* not short read */
			}
			error = 0;
			break;
		case SKEY_EQUAL:
			error = 0;
			break;
		case SKEY_NOT_READY:
#ifndef PORT_AMIGA
			if ((periph->periph_flags & PERIPH_REMOVABLE) != 0)
				periph->periph_flags &= ~PERIPH_MEDIA_LOADED;
#endif
			if ((xs->xs_control & XS_CTL_IGNORE_NOT_READY) != 0)
				return 0;
			if (sense->asc == 0x3A) {
#ifdef PORT_AMIGA
				if (periph->periph_flags & PERIPH_REMOVABLE)
					sd_media_unloaded(periph);
#endif
				error = ENODEV; /* Medium not present */
				if (xs->xs_control & XS_CTL_SILENT_NODEV)
					return error;
			} else
				error = EIO;
			if ((xs->xs_control & XS_CTL_SILENT) != 0)
				return error;
			break;
		case SKEY_ILLEGAL_REQUEST:
			if ((xs->xs_control &
			     XS_CTL_IGNORE_ILLEGAL_REQUEST) != 0)
				return 0;
			/*
			 * Handle the case where a device reports
			 * Logical Unit Not Supported during discovery.
			 */
			if ((xs->xs_control & XS_CTL_DISCOVERY) != 0 &&
			    sense->asc == 0x25 &&
			    sense->ascq == 0x00)
				return EINVAL;
			if ((xs->xs_control & XS_CTL_SILENT) != 0)
				return EIO;
			error = EINVAL;
			break;
		case SKEY_UNIT_ATTENTION:
			if (sense->asc == 0x29 &&
			    sense->ascq == 0x00) {
				/* device or bus reset */
				return ERESTART;
			}
#ifdef PORT_AMIGA
			if ((periph->periph_flags & PERIPH_REMOVABLE) != 0)
				sd_media_unloaded(periph);
#else
			if ((periph->periph_flags & PERIPH_REMOVABLE) != 0)
				periph->periph_flags &= ~PERIPH_MEDIA_LOADED;
#endif
			if ((xs->xs_control &
			     XS_CTL_IGNORE_MEDIA_CHANGE) != 0 ||
				/* XXX Should reupload any transient state. */
				(periph->periph_flags &
				 PERIPH_REMOVABLE) == 0) {
				return ERESTART;
			}
			if ((xs->xs_control & XS_CTL_SILENT) != 0)
				return EIO;
			error = EIO;
			break;
		case SKEY_DATA_PROTECT:
			error = EROFS;
			break;
		case SKEY_BLANK_CHECK:
			error = 0;
			break;
		case SKEY_ABORTED_COMMAND:
			if (xs->xs_retries != 0) {
				xs->xs_retries--;
				error = ERESTART;
			} else
				error = EIO;
			break;
		case SKEY_VOLUME_OVERFLOW:
			error = ENOSPC;
			break;
		default:
			error = EIO;
			break;
		}

#if 0
		/* Print verbose decode if appropriate and possible */
		if ((key == 0) ||
		    ((xs->xs_control & XS_CTL_SILENT) != 0) ||
		    (scsipi_print_sense(xs, 0) != 0))
			return error;
#endif

		/* Print brief(er) sense information */
		scsipi_printaddr(periph);
		printf("%s", error_mes[key - 1]);
		if ((sense->response_code & SSD_RCODE_VALID) != 0) {
			switch (key) {
			case SKEY_NOT_READY:
			case SKEY_ILLEGAL_REQUEST:
			case SKEY_UNIT_ATTENTION:
			case SKEY_DATA_PROTECT:
				break;
			case SKEY_BLANK_CHECK:
				printf(", requested size: %d (decimal)",
				    info);
				break;
			case SKEY_ABORTED_COMMAND:
				if (xs->xs_retries)
					printf(", retrying");
				printf(", cmd 0x%x, info 0x%x",
				    xs->cmd->opcode, info);
				break;
			default:
				printf(", info = %d (decimal)", info);
			}
		}
		if (sense->extra_len != 0) {
			int n;
			printf(", data =");
			for (n = 0; n < sense->extra_len; n++)
				printf(" %02x",
				    sense->csi[n]);
		}
		printf("\n");
		return error;

	/*
	 * Some other code, just report it
	 */
	default:
#ifdef USE_SERIAL_OUTPUT
#if    defined(SCSIDEBUG) || defined(DEBUG)
	{
		static const char *uc = "undecodable sense error";
		int i;
		u_int8_t *cptr = (u_int8_t *) sense;
		scsipi_printaddr(periph);
		if (xs->cmd == &xs->cmdstore) {
			printf("%s for opcode 0x%x, data=",
			    uc, xs->cmdstore.opcode);
		} else {
			printf("%s, data=", uc);
		}
		for (i = 0; i < sizeof (sense); i++)
			printf(" 0x%02x", *(cptr++) & 0xff);
		printf("\n");
	}
#else
		scsipi_printaddr(periph);
		printf("Sense Error Code 0x%x",
			SSD_RCODE(sense->response_code));
		if ((sense->response_code & SSD_RCODE_VALID) != 0) {
			struct scsi_sense_data_unextended *usense =
			    (struct scsi_sense_data_unextended *)sense;
			printf(" at block no. %d (decimal)",
			    _3btol(usense->block));
		}
		printf("\n");
#endif
#endif /* USE_SERIAL_OUTPUT */
		return EIO;
	}
}

static int
scsipi_inquiry3_ok(const struct scsipi_inquiry_data *ib)
{
#if 0
	for (size_t i = 0; i < __arraycount(scsipi_inquiry3_quirk); i++) {
		const struct scsipi_inquiry3_pattern *q =
		    &scsipi_inquiry3_quirk[i];
#define MATCH(field) \
    (q->field[0] ? memcmp(ib->field, q->field, sizeof(ib->field)) == 0 : 1)
		if (MATCH(vendor) && MATCH(product) && MATCH(revision))
			return 0;
	}
#endif
	return 1;
}


/*
 * scsipi_inquire:
 *
 *	Ask the device about itself.
 */
int
scsipi_inquire(struct scsipi_periph *periph, struct scsipi_inquiry_data *inqbuf,
    int flags)
{
	struct scsipi_inquiry cmd;
	int error;
	int retries;

	if (flags & XS_CTL_DISCOVERY)
		retries = 0;
	else
		retries = SCSIPIRETRIES;

	/*
	 * If we request more data than the device can provide, it SHOULD just
	 * return a short response.  However, some devices error with an
	 * ILLEGAL REQUEST sense code, and yet others have even more special
	 * failure modes (such as the GL641USB flash adapter, which goes loony
	 * and sends corrupted CRCs).  To work around this, and to bring our
	 * behavior more in line with other OSes, we do a shorter inquiry,
	 * covering all the SCSI-2 information, first, and then request more
	 * data iff the "additional length" field indicates there is more.
	 * - mycroft, 2003/10/16
	 */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = INQUIRY;
	cmd.length = SCSIPI_INQUIRY_LENGTH_SCSI2;
	error = scsipi_command(periph, (void *)&cmd, sizeof(cmd),
	    (void *)inqbuf, SCSIPI_INQUIRY_LENGTH_SCSI2, retries,
	    3000, NULL, flags | XS_CTL_DATA_IN);
	if (!error &&
	    inqbuf->additional_length > SCSIPI_INQUIRY_LENGTH_SCSI2 - 4) {
	    if (scsipi_inquiry3_ok(inqbuf)) {
#if 0
printf("inquire: addlen=%d, retrying\n", inqbuf->additional_length);
#endif
		cmd.length = SCSIPI_INQUIRY_LENGTH_SCSI3;
		error = scsipi_command(periph, (void *)&cmd, sizeof(cmd),
		    (void *)inqbuf, SCSIPI_INQUIRY_LENGTH_SCSI3, retries,
		    1000, NULL, flags | XS_CTL_DATA_IN);
#if 0
printf("inquire: error=%d\n", error);
#endif
	    }
	}

#ifdef SCSI_OLD_NOINQUIRY
	/*
	 * Kludge for the Adaptec ACB-4000 SCSI->MFM translator.
	 * This board doesn't support the INQUIRY command at all.
	 */
	if (error == EINVAL || error == EACCES) {
		/*
		 * Conjure up an INQUIRY response.
		 */
		inqbuf->device = (error == EINVAL ?
			 SID_QUAL_LU_PRESENT :
			 SID_QUAL_LU_NOTPRESENT) | T_DIRECT;
		inqbuf->dev_qual2 = 0;
		inqbuf->version = 0;
		inqbuf->response_format = SID_FORMAT_SCSI1;
		inqbuf->additional_length = SCSIPI_INQUIRY_LENGTH_SCSI2 - 4;
		inqbuf->flags1 = inqbuf->flags2 = inqbuf->flags3 = 0;
#ifdef PORT_AMIGA
		CopyMem("ADAPTEC ACB-4000            ", inqbuf->vendor, 28);
#else
		memcpy(inqbuf->vendor, "ADAPTEC ACB-4000            ", 28);
#endif
		error = 0;
	}

	/*
	 * Kludge for the Emulex MT-02 SCSI->QIC translator.
	 * This board gives an empty response to an INQUIRY command.
	 */
	else if (error == 0 &&
	    inqbuf->device == (SID_QUAL_LU_PRESENT | T_DIRECT) &&
	    inqbuf->dev_qual2 == 0 &&
	    inqbuf->version == 0 &&
	    inqbuf->response_format == SID_FORMAT_SCSI1) {
		/*
		 * Fill out the INQUIRY response.
		 */
		inqbuf->device = (SID_QUAL_LU_PRESENT | T_SEQUENTIAL);
		inqbuf->dev_qual2 = SID_REMOVABLE;
		inqbuf->additional_length = SCSIPI_INQUIRY_LENGTH_SCSI2 - 4;
		inqbuf->flags1 = inqbuf->flags2 = inqbuf->flags3 = 0;
#ifdef PORT_AMIGA
		CopyMem("EMULEX  MT-02 QIC           ", inqbuf->vendor, 28);
#else
		memcpy(inqbuf->vendor, "EMULEX  MT-02 QIC           ", 28);
#endif
	}
#endif /* SCSI_OLD_NOINQUIRY */

	return error;
}

/*
 * scsipi_mode_sense, scsipi_mode_sense_big:
 *	get a sense page from a device
 */

int
scsipi_mode_sense(struct scsipi_periph *periph, int byte2, int page,
    struct scsi_mode_parameter_header_6 *data, int len, int flags, int retries,
    int timeout)
{
	struct scsi_mode_sense_6 cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = SCSI_MODE_SENSE_6;
	cmd.byte2 = byte2;
	cmd.page = page;
	cmd.length = len & 0xff;

#if 0
	return scsipi_command(periph, (void *)&cmd, sizeof(cmd),
	    (void *)data, len, retries, timeout, NULL, flags | XS_CTL_DATA_IN);
#else
	int rc = scsipi_command(periph, (void *)&cmd, sizeof(cmd),
	    (void *)data, len, retries, timeout, NULL, flags | XS_CTL_DATA_IN);
        printf("scsipi_command returned %d\n", rc);
        return (rc);
#endif
}

/*
 * Issue a request sense for the given scsipi_xfer. Called when the xfer
 * returns with a CHECK_CONDITION status. Must be called in valid thread
 * context.
 */

static void
scsipi_request_sense(struct scsipi_xfer *xs)
{
	struct scsipi_periph *periph = xs->xs_periph;
	int flags, error;
	struct scsi_request_sense cmd;

	periph->periph_flags |= PERIPH_SENSE;

	/* if command was polling, request sense will too */
	flags = xs->xs_control & XS_CTL_POLL;
	/* Polling commands can't sleep */
	if (flags)
		flags |= XS_CTL_NOSLEEP;

	flags |= XS_CTL_REQSENSE | XS_CTL_URGENT | XS_CTL_DATA_IN |
	    XS_CTL_THAW_PERIPH | XS_CTL_FREEZE_PERIPH;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = SCSI_REQUEST_SENSE;
	cmd.length = sizeof(struct scsi_sense_data);

	error = scsipi_command(periph, (void *)&cmd, sizeof(cmd),
	    (void *)&xs->sense.scsi_sense, sizeof(struct scsi_sense_data),
	    0, 1000, NULL, flags);
	periph->periph_flags &= ~PERIPH_SENSE;
	periph->periph_xscheck = NULL;
	switch (error) {
	case 0:
		/* we have a valid sense */
		xs->error = XS_SENSE;
		return;
	case EINTR:
		/* REQUEST_SENSE interrupted by bus reset. */
		xs->error = XS_RESET;
                printf("bus reset interrupted request sense\n");
		return;
	case EIO:
		 /* request sense couldn't be performed */
		/*
		 * XXX this isn't quite right but we don't have anything
		 * better for now
		 */
		xs->error = XS_DRIVER_STUFFUP;
                printf("request sense driver stuffup\n");
		return;
	default:
		 /* Notify that request sense failed. */
		xs->error = XS_DRIVER_STUFFUP;
		scsipi_printaddr(periph);
		printf("request sense failed with error %d\n", error);
		return;
	}
}

/*
 * scsipi_test_unit_ready:
 *
 *	Issue a `test unit ready' request.
 */
int
scsipi_test_unit_ready(struct scsipi_periph *periph, int flags)
{
	struct scsi_test_unit_ready cmd;
	int retries;

	/* some ATAPI drives don't support TEST UNIT READY. Sigh */
	if (periph->periph_quirks & PQUIRK_NOTUR)
		return 0;

	if (flags & XS_CTL_DISCOVERY)
		retries = 0;
	else
		retries = SCSIPIRETRIES;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = SCSI_TEST_UNIT_READY;

	return scsipi_command(periph, (void *)&cmd, sizeof(cmd), 0, 0,
	    retries, 5000, NULL, flags);
}

/*
 * scsipi_set_xfer_mode:
 *
 *	Set the xfer mode for the specified I_T Nexus.
 */
void
scsipi_set_xfer_mode(struct scsipi_channel *chan, int target, int immed)
{
	struct scsipi_xfer_mode xm;
	struct scsipi_periph *itperiph;
	int lun;

	/*
	 * Go to the minimal xfer mode.
	 */
	xm.xm_target = target;
	xm.xm_mode = 0;
	xm.xm_period = 0;			/* ignored */
	xm.xm_offset = 0;			/* ignored */

	/*
	 * Find the first LUN we know about on this I_T Nexus.
	 */
	for (itperiph = NULL, lun = 0; lun < chan->chan_nluns; lun++) {
		itperiph = scsipi_lookup_periph(chan, target, lun);
		if (itperiph != NULL)
			break;
	}
	if (itperiph != NULL) {
		xm.xm_mode = itperiph->periph_cap;
		/*
		 * Now issue the request to the adapter.
		 */
		scsipi_adapter_request(chan, ADAPTER_REQ_SET_XFER_MODE, &xm);
		/*
		 * If we want this to happen immediately, issue a dummy
		 * command, since most adapters can't really negotiate unless
		 * they're executing a job.
		 */
		if (immed != 0) {
			(void) scsipi_test_unit_ready(itperiph,
			    XS_CTL_DISCOVERY | XS_CTL_IGNORE_ILLEGAL_REQUEST |
			    XS_CTL_IGNORE_NOT_READY |
			    XS_CTL_IGNORE_MEDIA_CHANGE);
		}
	}
}

#ifdef SCSIPI_DEBUG
void
show_mem(u_char *address, int num)
{
        int x;

        printf("------------------------------");
        for (x = 0; x < num; x++) {
                if ((x % 16) == 0)
                        printf("\n%03d: ", x);
                printf("%02x ", *address++);
        }
        printf("\n------------------------------\n");
}

/*
 * Given a scsipi_xfer, dump the request, in all its glory
 */
void
show_scsipi_xs(struct scsipi_xfer *xs)
{

        printf("xs(%p): ", xs);
        printf("xs_control(0x%08x)", xs->xs_control);
        printf("xs_status(0x%08x)", xs->xs_status);
        printf("periph(%p)", xs->xs_periph);
        printf("retr(0x%x)", xs->xs_retries);
        printf("timo(0x%x)", xs->timeout);
        printf("cmd(%p)", xs->cmd);
        printf("len(0x%x)", xs->cmdlen);
        printf("data(%p)", xs->data);
        printf("len(0x%x)", xs->datalen);
        printf("res(0x%x)", xs->resid);
        printf("err(0x%x)", xs->error);
        printf("bp(%p)", xs->bp);
        show_scsipi_cmd(xs);
}

void
show_scsipi_cmd(struct scsipi_xfer *xs)
{
        u_char *b = (u_char *) xs->cmd;
        int i = 0;

        scsipi_printaddr(xs->xs_periph);
        printf(" command: ");

        if ((xs->xs_control & XS_CTL_RESET) == 0) {
                while (i < xs->cmdlen) {
                        if (i)
                                printf(",");
                        printf("0x%x", b[i++]);
                }
                printf("-[%d bytes]\n", xs->datalen);
                if (xs->datalen)
                        show_mem(xs->data, 64);
        } else
                printf("-RESET-\n");
}
#endif
