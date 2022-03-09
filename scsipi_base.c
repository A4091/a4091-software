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

#include "port.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <inline/exec.h>

#include "scsipiconf.h"
#include "scsi_disk.h"
#include "scsipi_disk.h"
#include "scsipi_base.h"
#include "scsi_all.h"
#include "port_bsd.h"
#include "siopreg.h"
#include "siopvar.h"
#include "scsi_message.h"
#include "sd.h"

#undef SCSIPI_DEBUG

#define ERESTART 100  // Needs restart

static void scsipi_run_queue(struct scsipi_channel *chan);

extern struct ExecBase *SysBase;

#undef CDH_DIRECT_REQUEST

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
		TAILQ_INSERT_HEAD(&chan->chan_queue, xs, channel_q);
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
			TAILQ_INSERT_AFTER(&chan->chan_queue, qxs, xs,
			    channel_q);
			goto out;
		}
	}
	TAILQ_INSERT_TAIL(&chan->chan_queue, xs, channel_q);
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
    struct scsipi_xfer *xs = AllocMem(sizeof (*xs), MEMF_CLEAR | MEMF_PUBLIC);
//  callout_init(&xs->xs_callout, 0);
    xs->xs_periph = periph;
    xs->xs_control = flags;
    xs->xs_status = 0;
    return (xs);
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

	scsipi_update_timeouts(xs);

//	(chan->chan_bustype->bustype_cmd)(xs);

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
			panic("scsipi_execute_xs");
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
			panic("scsipi_execute_xs");
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
        uint timeout = 60;
	while ((xs->xs_status & XS_STS_DONE) == 0) {
		if (poll) {
			scsipi_printaddr(periph);
			printf("polling command not done\n");
			panic("scsipi_execute_xs");
		}
//		cv_wait(xs_cv(xs), chan_mtx(chan));

                Delay(1);
                if (timeout-- == 0) {
                    printf("CDH: XS timeout\n");
                    break;
                }
	}

	/*
	 * Command is complete.  scsipi_done() has awakened us to perform
	 * the error handling.
	 */
	mutex_exit(chan_mtx(chan));
        printf("CDH: Forced sd complete with error because we can't poll yet\n");
        sd_complete(xs, 1); error = 1;
//	error = scsipi_complete(xs);
	if (error == ERESTART)
		goto restarted;

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
//	scsipi_put_xs(xs);
	mutex_exit(chan_mtx(chan));

	/*
	 * Kick the queue, keep it running in case it stopped for some
	 * reason.
	 */
	scsipi_run_queue(chan);

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
//      struct scsipi_adapter *adapt = chan->chan_adapter;
        int i;

#if 0
        /* Initialize shared data. */
        scsipi_init();
#endif

        /* Initialize the queues. */
        TAILQ_INIT(&chan->chan_queue);
        TAILQ_INIT(&chan->chan_complete);

        for (i = 0; i < SCSIPI_CHAN_PERIPH_BUCKETS; i++)
                LIST_INIT(&chan->chan_periphtab[i]);

#if 0
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
    memcpy(&xs->cmdstore, cmd, cmdlen);
    xs->cmd = &xs->cmdstore;
    xs->cmdlen = cmdlen;
    xs->data = data_addr;
    xs->datalen = datalen;
    xs->xs_retries = retries;
    xs->timeout = timeout;
    xs->bp = bp;

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

    printf("scsipi_adapter_request(%u)\n", req);

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
		if ((chan->chan_flags & SCSIPI_CHAN_TACTIVE) == 0) {
			mutex_exit(chan_mtx(chan));
			scsipi_adapter_request(chan,
			    ADAPTER_REQ_GROW_RESOURCES, NULL);
			mutex_enter(chan_mtx(chan));
			return scsipi_get_resource(chan);
		}
		/*
		 * ask the channel thread to do it. It'll have to thaw the
		 * queue
		 */
#if 0
		scsipi_channel_freeze_locked(chan, 1);
#endif
		chan->chan_tflags |= SCSIPI_CHANT_GROWRES;
//		cv_broadcast(chan_cv_complete(chan));
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
		/*
		 * If the channel is frozen, we can't do any work right
		 * now.
		 */
		if (chan->chan_qfreeze != 0) {
			break;
		}

		/*
		 * Look for work to do, and make sure we can do it.
		 */
		for (xs = TAILQ_FIRST(&chan->chan_queue); xs != NULL;
		     xs = TAILQ_NEXT(xs, channel_q)) {
			periph = xs->xs_periph;

			if ((periph->periph_sent >= periph->periph_openings) ||
			    periph->periph_qfreeze != 0 ||
			    (periph->periph_flags & PERIPH_UNTAG) != 0)
				continue;

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
		TAILQ_REMOVE(&chan->chan_queue, xs, channel_q);

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

printf("CDH: scsipi_done %p\n", xs);
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
                sd_complete(xs, 0);
		goto out;
	}

#if 0
	/*
	 * There is an error on this xfer.  Put it on the channel's
	 * completion queue, and wake up the completion thread.
	 */
	TAILQ_INSERT_TAIL(&chan->chan_complete, xs, channel_q);
	cv_broadcast(chan_cv_complete(chan));
#endif
        sd_complete(xs, 1);

 out:
	/*
	 * If there are more xfers on the channel's queue, attempt to
	 * run them.
	 */
	scsipi_run_queue(chan);
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
