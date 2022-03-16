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
#include "siopreg.h"
#include "siopvar.h"
#include "scsi_message.h"
#include "sd.h"

#undef SCSIPI_DEBUG

#define ERESTART 100  // Needs restart

static void scsipi_run_queue(struct scsipi_channel *chan);
static int scsipi_complete(struct scsipi_xfer *xs);
static void scsipi_request_sense(struct scsipi_xfer *xs);
static void scsipi_completion_poll(struct scsipi_channel *chan);

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
    callout_init(&xs->xs_callout, 0);
    xs->xs_periph = periph;
    xs->xs_control = flags;
    xs->xs_status = 0;
    xs->amiga_ior = NULL;
    xs->amiga_sdirect = NULL;
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
        struct siop_softc *sc = device_private(chan->chan_adapter->adapt_dev);
        printf("SYNC -- poll for completion\n");

	while ((xs->xs_status & XS_STS_DONE) == 0) {
#ifdef PORT_AMIGA
                /*
                 * Need to run interrupt message handling here because
                 * this task runs in the same context as the normal interrupt
                 * message handling.
                 */
                void irq_poll(int gotint, struct siop_softc *sc);
                irq_poll(0, sc);
#else
		if (poll) {
			scsipi_printaddr(periph);
			panic("scsipi_execute_xs");
		}
		cv_wait(xs_cv(xs), chan_mtx(chan));
#endif
                Delay(1);
                if (timeout-- == 0) {
                    printf("CDH: Poll timeout\n");
                    break;
                }
	}

	/*
	 * Command is complete.  scsipi_done() has awakened us to perform
	 * the error handling.
	 */
	mutex_exit(chan_mtx(chan));
	error = scsipi_complete(xs);
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

#if 0
    printf("CDH: scsipi_adapter_request(%u)\n", req);
#endif

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

printf("CDH: scsipi_done(%p)\n", xs);
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
printf("CDH: periph sense required ior=%p\n", xs->amiga_ior);
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
	TAILQ_INSERT_TAIL(&chan->chan_complete, xs, channel_q);
#if 0
	cv_broadcast(chan_cv_complete(chan));
#endif
        scsipi_completion_poll(chan);

 out:
	/*
	 * If there are more xfers on the channel's queue, attempt to
	 * run them.
	 */
	scsipi_run_queue(chan);
}

static void
scsipi_completion_poll(struct scsipi_channel *chan)
{
    struct scsipi_xfer *xs;

    chan->chan_flags |= SCSIPI_CHAN_TACTIVE;
    while ((xs = TAILQ_FIRST(&chan->chan_complete)) != NULL) {
        if (chan->chan_tflags & SCSIPI_CHANT_GROWRES) {
            /* attempt to get more openings for this channel */
            chan->chan_tflags &= ~SCSIPI_CHANT_GROWRES;
            mutex_exit(chan_mtx(chan));
            scsipi_adapter_request(chan,
                ADAPTER_REQ_GROW_RESOURCES, NULL);
#if 0
            scsipi_channel_thaw(chan, 1);
#endif
            if (chan->chan_tflags & SCSIPI_CHANT_GROWRES)
                delay(100000);
            mutex_enter(chan_mtx(chan));
            continue;
        }
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
        if (xs) {
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
    }
    chan->chan_flags &= ~SCSIPI_CHAN_TACTIVE;
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
                error = xs->error;
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
#if 0
				/* XXX: quite extreme */
				kpause("xsbusy", false, hz, chan_mtx(chan));
#else
                                printf("CDH: poll delay\n");
                                delay(1000000);
#endif
#if 0
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
#if 0
		if (scsipi_lookup_periph(chan, periph->periph_target,
		    periph->periph_lun) && xs->xs_retries != 0) {
			xs->xs_retries--;
			error = ERESTART;
		} else
#endif
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

        sd_complete(xs, xs->error);

#if 0
	mutex_enter(chan_mtx(chan));
#endif
#if 0
	if (xs->xs_control & XS_CTL_ASYNC)
		scsipi_put_xs(xs);
#endif
	mutex_exit(chan_mtx(chan));

	return error;
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

printf("CDH: asking for periph sense ior=%p\n", xs->amiga_ior);
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
		return;
	case EIO:
		 /* request sense couldn't be performed */
		/*
		 * XXX this isn't quite right but we don't have anything
		 * better for now
		 */
		xs->error = XS_DRIVER_STUFFUP;
		return;
	default:
		 /* Notify that request sense failed. */
		xs->error = XS_DRIVER_STUFFUP;
		scsipi_printaddr(periph);
		printf("request sense failed with error %d\n", error);
		return;
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
