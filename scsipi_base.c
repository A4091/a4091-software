/*	$NetBSD: scsipi_base.c,v 1.189 2022/04/09 23:38:32 riastradh Exp $	*/

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
#ifdef PORT_AMIGA
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

extern struct ExecBase *SysBase;

#else /* !PORT_AMIGA */
#include <sys/cdefs.h>

__KERNEL_RCSID(0, "$NetBSD: scsipi_base.c,v 1.187 2020/09/17 01:19:41 jakllsch Exp $");

#ifdef _KERNEL_OPT
#include "opt_scsi.h"
#endif

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/buf.h>
#include <sys/uio.h>
#include <sys/malloc.h>
#include <sys/pool.h>
#include <sys/errno.h>
#include <sys/device.h>
#include <sys/proc.h>
#include <sys/kthread.h>
#include <sys/hash.h>
#include <sys/atomic.h>

#include <dev/scsipi/scsi_sdt.h>
#include <dev/scsipi/scsi_spc.h>
#include <dev/scsipi/scsipi_all.h>
#include <dev/scsipi/scsipi_disk.h>
#include <dev/scsipi/scsipiconf.h>
#include <dev/scsipi/scsipi_base.h>

#include <dev/scsipi/scsi_all.h>
#include <dev/scsipi/scsi_message.h>

#include <machine/param.h>

SDT_PROVIDER_DEFINE(scsi);

SDT_PROBE_DEFINE3(scsi, base, tag, get,
    "struct scsipi_xfer *"/*xs*/, "uint8_t"/*tag*/, "uint8_t"/*type*/);
SDT_PROBE_DEFINE3(scsi, base, tag, put,
    "struct scsipi_xfer *"/*xs*/, "uint8_t"/*tag*/, "uint8_t"/*type*/);

SDT_PROBE_DEFINE3(scsi, base, adapter, request__start,
    "struct scsipi_channel *"/*chan*/,
    "scsipi_adapter_req_t"/*req*/,
    "void *"/*arg*/);
SDT_PROBE_DEFINE3(scsi, base, adapter, request__done,
    "struct scsipi_channel *"/*chan*/,
    "scsipi_adapter_req_t"/*req*/,
    "void *"/*arg*/);

SDT_PROBE_DEFINE1(scsi, base, queue, batch__start,
    "struct scsipi_channel *"/*chan*/);
SDT_PROBE_DEFINE2(scsi, base, queue, run,
    "struct scsipi_channel *"/*chan*/,
    "struct scsipi_xfer *"/*xs*/);
SDT_PROBE_DEFINE1(scsi, base, queue, batch__done,
    "struct scsipi_channel *"/*chan*/);

SDT_PROBE_DEFINE1(scsi, base, xfer, execute,  "struct scsipi_xfer *"/*xs*/);
SDT_PROBE_DEFINE1(scsi, base, xfer, enqueue,  "struct scsipi_xfer *"/*xs*/);
SDT_PROBE_DEFINE1(scsi, base, xfer, done,  "struct scsipi_xfer *"/*xs*/);
SDT_PROBE_DEFINE1(scsi, base, xfer, redone,  "struct scsipi_xfer *"/*xs*/);
SDT_PROBE_DEFINE1(scsi, base, xfer, complete,  "struct scsipi_xfer *"/*xs*/);
SDT_PROBE_DEFINE1(scsi, base, xfer, restart,  "struct scsipi_xfer *"/*xs*/);
SDT_PROBE_DEFINE1(scsi, base, xfer, free,  "struct scsipi_xfer *"/*xs*/);
#endif
static int	scsipi_complete(struct scsipi_xfer *);
static void	scsipi_request_sense(struct scsipi_xfer *);
static int	scsipi_enqueue(struct scsipi_xfer *);
static void	scsipi_run_queue(struct scsipi_channel *chan);

#ifdef PORT_AMIGA
static void scsipi_adapter_request(struct scsipi_channel *chan,
	                           scsipi_adapter_req_t req, void *arg);
static struct scsipi_xfer *scsipi_get_xs(struct scsipi_periph *periph,
                                         int flags);
#else
static void	scsipi_completion_thread(void *);
#endif

static void	scsipi_get_tag(struct scsipi_xfer *);
static void	scsipi_put_tag(struct scsipi_xfer *);

static int	scsipi_get_resource(struct scsipi_channel *);
static void	scsipi_put_resource(struct scsipi_channel *);

#ifndef PORT_AMIGA
static void	scsipi_async_event_max_openings(struct scsipi_channel *,
		    struct scsipi_max_openings *);
static void	scsipi_async_event_channel_reset(struct scsipi_channel *);

static void	scsipi_channel_freeze_locked(struct scsipi_channel *, int);

static void	scsipi_adapter_lock(struct scsipi_adapter *adapt);
static void	scsipi_adapter_unlock(struct scsipi_adapter *adapt);

static void	scsipi_update_timeouts(struct scsipi_xfer *xs);

static struct pool scsipi_xfer_pool;

int scsipi_xs_count = 0;

/*
 * scsipi_init:
 *
 *	Called when a scsibus or atapibus is attached to the system
 *	to initialize shared data structures.
 */
void
scsipi_init(void)
{
	static int scsipi_init_done;

	if (scsipi_init_done)
		return;
	scsipi_init_done = 1;

	/* Initialize the scsipi_xfer pool. */
	pool_init(&scsipi_xfer_pool, sizeof(struct scsipi_xfer), 0,
	    0, 0, "scxspl", NULL, IPL_BIO);
	pool_prime(&scsipi_xfer_pool, 1);

	scsipi_ioctl_init();
}
#endif /* !PORT_AMIGA */

/*
 * scsipi_channel_init:
 *
 *	Initialize a scsipi_channel when it is attached.
 */
int
scsipi_channel_init(struct scsipi_channel *chan)
{
#ifdef PORT_AMIGA
	int i;
	chan->chan_xs_free = NULL;
#else /* !PORT_AMIGA */
	struct scsipi_adapter *adapt = chan->chan_adapter;
	int i;

	/* Initialize shared data. */
	scsipi_init();
#endif /* !PORT_AMIGA */

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

#ifndef PORT_AMIGA
/*
 * scsipi_channel_shutdown:
 *
 *	Shutdown a scsipi_channel.
 */
void
scsipi_channel_shutdown(struct scsipi_channel *chan)
{

	mutex_enter(chan_mtx(chan));
	/*
	 * Shut down the completion thread.
	 */
	chan->chan_tflags |= SCSIPI_CHANT_SHUTDOWN;
	cv_broadcast(chan_cv_complete(chan));

	/*
	 * Now wait for the thread to exit.
	 */
	while (chan->chan_thread != NULL)
		cv_wait(chan_cv_thread(chan), chan_mtx(chan));
	mutex_exit(chan_mtx(chan));
}
#endif /* PORT_AMIGA */

static uint32_t
scsipi_chan_periph_hash(uint64_t t, uint64_t l)
{
#ifdef PORT_AMIGA
	(void)t; (void)l;
        return (0);
#else
	uint32_t hash;

	hash = hash32_buf(&t, sizeof(t), HASH32_BUF_INIT);
	hash = hash32_buf(&l, sizeof(l), hash);

	return hash & SCSIPI_CHAN_PERIPH_HASHMASK;
#endif
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
	(void)chan;
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

#ifndef PORT_AMIGA
struct scsipi_periph *
scsipi_lookup_periph_locked(struct scsipi_channel *chan, int target, int lun)
{
	return scsipi_lookup_periph_internal(chan, target, lun, false);
}

#endif

struct scsipi_periph *
scsipi_lookup_periph(struct scsipi_channel *chan, int target, int lun)
{
	return scsipi_lookup_periph_internal(chan, target, lun, true);
}


#ifdef PORT_AMIGA
static
#endif
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

#ifndef PORT_AMIGA
	if (chan->chan_flags & SCSIPI_CHAN_OPENINGS) {
		if (chan->chan_openings > 0) {
			chan->chan_openings--;
			return 1;
		}
		return 0;
	}
#endif

	if (adapt->adapt_openings > 0) {
		adapt->adapt_openings--;
		return 1;
	}
	return 0;
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
		cv_broadcast(chan_cv_complete(chan));
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

#ifndef PORT_AMIGA
	if (chan->chan_flags & SCSIPI_CHAN_OPENINGS)
		chan->chan_openings++;
	else
#endif
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
	if (word == PERIPH_NTAGWORDS) {
#ifdef DIAGNOSTIC
		scsipi_printaddr(periph);
#endif
		printf("no free tags\n");
		panic("scsipi_get_tag");
	}

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
 * scsipi_get_xs:
 *
 *	Allocate an xfer descriptor and associate it with the
 *	specified peripheral.  If the peripheral has no more
 *	available command openings, we either block waiting for
 *	one to become available, or fail.
 *
 *	When this routine is called with the channel lock held
 *	the flags must include XS_CTL_NOSLEEP.
 */
#ifdef PORT_AMIGA
static
#endif
struct scsipi_xfer *
scsipi_get_xs(struct scsipi_periph *periph, int flags)
{
#ifdef PORT_AMIGA
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

    chan->chan_active++;

#if 0
    printf("get_xs(%p) active=%u\n", xs, chan->chan_active);
#endif
    return (xs);
#else /* !PORT_AMIGA */
	struct scsipi_xfer *xs;
	bool lock = (flags & XS_CTL_NOSLEEP) == 0;

	SC_DEBUG(periph, SCSIPI_DB3, ("scsipi_get_xs\n"));

	KASSERT(!cold);

#ifdef DIAGNOSTIC
	/*
	 * URGENT commands can never be ASYNC.
	 */
	if ((flags & (XS_CTL_URGENT|XS_CTL_ASYNC)) ==
	    (XS_CTL_URGENT|XS_CTL_ASYNC)) {
		scsipi_printaddr(periph);
		printf("URGENT and ASYNC\n");
		panic("scsipi_get_xs");
	}
#endif

	/*
	 * Wait for a command opening to become available.  Rules:
	 *
	 *	- All xfers must wait for an available opening.
	 *	  Exception: URGENT xfers can proceed when
	 *	  active == openings, because we use the opening
	 *	  of the command we're recovering for.
	 *	- if the periph has sense pending, only URGENT & REQSENSE
	 *	  xfers may proceed.
	 *
	 *	- If the periph is recovering, only URGENT xfers may
	 *	  proceed.
	 *
	 *	- If the periph is currently executing a recovery
	 *	  command, URGENT commands must block, because only
	 *	  one recovery command can execute at a time.
	 */
	if (lock)
		mutex_enter(chan_mtx(periph->periph_channel));
	for (;;) {
		if (flags & XS_CTL_URGENT) {
			if (periph->periph_active > periph->periph_openings)
				goto wait_for_opening;
			if (periph->periph_flags & PERIPH_SENSE) {
				if ((flags & XS_CTL_REQSENSE) == 0)
					goto wait_for_opening;
			} else {
				if ((periph->periph_flags &
				    PERIPH_RECOVERY_ACTIVE) != 0)
					goto wait_for_opening;
				periph->periph_flags |= PERIPH_RECOVERY_ACTIVE;
			}
			break;
		}
		if (periph->periph_active >= periph->periph_openings ||
		    (periph->periph_flags & PERIPH_RECOVERING) != 0)
			goto wait_for_opening;
		periph->periph_active++;
		KASSERT(mutex_owned(chan_mtx(periph->periph_channel)));
		break;

 wait_for_opening:
		if (flags & XS_CTL_NOSLEEP) {
			KASSERT(!lock);
			return NULL;
		}
		KASSERT(lock);
		SC_DEBUG(periph, SCSIPI_DB3, ("sleeping\n"));
		periph->periph_flags |= PERIPH_WAITING;
		cv_wait(periph_cv_periph(periph),
		    chan_mtx(periph->periph_channel));
	}
	if (lock)
		mutex_exit(chan_mtx(periph->periph_channel));

	SC_DEBUG(periph, SCSIPI_DB3, ("calling pool_get\n"));
	xs = pool_get(&scsipi_xfer_pool,
	    ((flags & XS_CTL_NOSLEEP) != 0 ? PR_NOWAIT : PR_WAITOK));
	if (xs == NULL) {
		if (lock)
			mutex_enter(chan_mtx(periph->periph_channel));
		if (flags & XS_CTL_URGENT) {
			if ((flags & XS_CTL_REQSENSE) == 0)
				periph->periph_flags &= ~PERIPH_RECOVERY_ACTIVE;
		} else
			periph->periph_active--;
		if (lock)
			mutex_exit(chan_mtx(periph->periph_channel));
		scsipi_printaddr(periph);
		printf("unable to allocate %sscsipi_xfer\n",
		    (flags & XS_CTL_URGENT) ? "URGENT " : "");
	}

	SC_DEBUG(periph, SCSIPI_DB3, ("returning\n"));

	if (xs != NULL) {
		memset(xs, 0, sizeof(*xs));
		callout_init(&xs->xs_callout, 0);
		xs->xs_periph = periph;
		xs->xs_control = flags;
		xs->xs_status = 0;
		if ((flags & XS_CTL_NOSLEEP) == 0)
			mutex_enter(chan_mtx(periph->periph_channel));
		TAILQ_INSERT_TAIL(&periph->periph_xferq, xs, device_q);
		KASSERT(mutex_owned(chan_mtx(periph->periph_channel)));
		if ((flags & XS_CTL_NOSLEEP) == 0)
			mutex_exit(chan_mtx(periph->periph_channel));
	}
	return xs;
#endif /* !PORT_AMIGA */
}

/*
 * scsipi_put_xs:
 *
 *	Release an xfer descriptor, decreasing the outstanding command
 *	count for the peripheral.  If there is a thread waiting for
 *	an opening, wake it up.  If not, kick any queued I/O the
 *	peripheral may have.
 *
 *	NOTE: Must be called with channel lock held
 */
#ifdef PORT_AMIGA
static
#endif
void
scsipi_put_xs(struct scsipi_xfer *xs)
{
#ifdef PORT_AMIGA
    /* pool put */
    struct scsipi_periph  *periph = xs->xs_periph;
    struct scsipi_channel *chan = periph->periph_channel;

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
#else /* !PORT_AMIGA */
	struct scsipi_periph *periph = xs->xs_periph;
	int flags = xs->xs_control;

	SDT_PROBE1(scsi, base, xfer, free,  xs);
	SC_DEBUG(periph, SCSIPI_DB3, ("scsipi_free_xs\n"));
	KASSERT(mutex_owned(chan_mtx(periph->periph_channel)));

	TAILQ_REMOVE(&periph->periph_xferq, xs, device_q);
	callout_destroy(&xs->xs_callout);
	pool_put(&scsipi_xfer_pool, xs);

#ifdef DIAGNOSTIC
	if ((periph->periph_flags & PERIPH_RECOVERY_ACTIVE) != 0 &&
	    periph->periph_active == 0) {
		scsipi_printaddr(periph);
		printf("recovery without a command to recovery for\n");
		panic("scsipi_put_xs");
	}
#endif

	if (flags & XS_CTL_URGENT) {
		if ((flags & XS_CTL_REQSENSE) == 0)
			periph->periph_flags &= ~PERIPH_RECOVERY_ACTIVE;
	} else
		periph->periph_active--;
	if (periph->periph_active == 0 &&
	    (periph->periph_flags & PERIPH_WAITDRAIN) != 0) {
		periph->periph_flags &= ~PERIPH_WAITDRAIN;
		cv_broadcast(periph_cv_active(periph));
	}

	if (periph->periph_flags & PERIPH_WAITING) {
		periph->periph_flags &= ~PERIPH_WAITING;
		cv_broadcast(periph_cv_periph(periph));
	} else {
		if (periph->periph_switch->psw_start != NULL &&
		    device_is_active(periph->periph_dev)) {
			SC_DEBUG(periph, SCSIPI_DB2,
			    ("calling private start()\n"));
			(*periph->periph_switch->psw_start)(periph);
		}
	}
#endif /* !PORT_AMIGA */
}

#ifdef PORT_AMIGA
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
#endif /* PORT_AMIGA */

#ifndef PORT_AMIGA
/*
 * scsipi_channel_freeze:
 *
 *	Freeze a channel's xfer queue.
 */
void
scsipi_channel_freeze(struct scsipi_channel *chan, int count)
{
	bool lock = chan_running(chan) > 0;

	if (lock)
		mutex_enter(chan_mtx(chan));
	chan->chan_qfreeze += count;
	if (lock)
		mutex_exit(chan_mtx(chan));
}

static void
scsipi_channel_freeze_locked(struct scsipi_channel *chan, int count)
{

	chan->chan_qfreeze += count;
}

/*
 * scsipi_channel_thaw:
 *
 *	Thaw a channel's xfer queue.
 */
void
scsipi_channel_thaw(struct scsipi_channel *chan, int count)
{
	bool lock = chan_running(chan) > 0;

	if (lock)
		mutex_enter(chan_mtx(chan));
	chan->chan_qfreeze -= count;
	/*
	 * Don't let the freeze count go negative.
	 *
	 * Presumably the adapter driver could keep track of this,
	 * but it might just be easier to do this here so as to allow
	 * multiple callers, including those outside the adapter driver.
	 */
	if (chan->chan_qfreeze < 0) {
		chan->chan_qfreeze = 0;
	}
	if (lock)
		mutex_exit(chan_mtx(chan));

	/*
	 * until the channel is running
	 */
	if (!lock)
		return;

	/*
	 * Kick the channel's queue here.  Note, we may be running in
	 * interrupt context (softclock or HBA's interrupt), so the adapter
	 * driver had better not sleep.
	 */
	if (chan->chan_qfreeze == 0)
		scsipi_run_queue(chan);
}

/*
 * scsipi_channel_timed_thaw:
 *
 *	Thaw a channel after some time has expired. This will also
 * 	run the channel's queue if the freeze count has reached 0.
 */
void
scsipi_channel_timed_thaw(void *arg)
{
	struct scsipi_channel *chan = arg;

	scsipi_channel_thaw(chan, 1);
}

/*
 * scsipi_periph_freeze:
 *
 *	Freeze a device's xfer queue.
 */
void
scsipi_periph_freeze_locked(struct scsipi_periph *periph, int count)
{

	periph->periph_qfreeze += count;
}

/*
 * scsipi_periph_thaw:
 *
 *	Thaw a device's xfer queue.
 */
void
scsipi_periph_thaw_locked(struct scsipi_periph *periph, int count)
{

	periph->periph_qfreeze -= count;
#ifdef DIAGNOSTIC
	if (periph->periph_qfreeze < 0) {
		static const char pc[] = "periph freeze count < 0";
		scsipi_printaddr(periph);
		printf("%s\n", pc);
		panic(pc);
	}
#endif
	if (periph->periph_qfreeze == 0 &&
	    (periph->periph_flags & PERIPH_WAITING) != 0)
		cv_broadcast(periph_cv_periph(periph));
}

void
scsipi_periph_freeze(struct scsipi_periph *periph, int count)
{

	mutex_enter(chan_mtx(periph->periph_channel));
	scsipi_periph_freeze_locked(periph, count);
	mutex_exit(chan_mtx(periph->periph_channel));
}

void
scsipi_periph_thaw(struct scsipi_periph *periph, int count)
{

	mutex_enter(chan_mtx(periph->periph_channel));
	scsipi_periph_thaw_locked(periph, count);
	mutex_exit(chan_mtx(periph->periph_channel));
}

/*
 * scsipi_periph_timed_thaw:
 *
 *	Thaw a device after some time has expired.
 */
void
scsipi_periph_timed_thaw(void *arg)
{
	struct scsipi_periph *periph = arg;
	struct scsipi_channel *chan = periph->periph_channel;

	callout_stop(&periph->periph_callout);

	mutex_enter(chan_mtx(chan));
	scsipi_periph_thaw_locked(periph, 1);
	if ((periph->periph_channel->chan_flags & SCSIPI_CHAN_TACTIVE) == 0) {
		/*
		 * Kick the channel's queue here.  Note, we're running in
		 * interrupt context (softclock), so the adapter driver
		 * had better not sleep.
		 */
		mutex_exit(chan_mtx(chan));
		scsipi_run_queue(periph->periph_channel);
	} else {
		/*
		 * Tell the completion thread to kick the channel's queue here.
		 */
		periph->periph_channel->chan_tflags |= SCSIPI_CHANT_KICK;
		cv_broadcast(chan_cv_complete(chan));
		mutex_exit(chan_mtx(chan));
	}
}

/*
 * scsipi_wait_drain:
 *
 *	Wait for a periph's pending xfers to drain.
 */
void
scsipi_wait_drain(struct scsipi_periph *periph)
{
	struct scsipi_channel *chan = periph->periph_channel;

	mutex_enter(chan_mtx(chan));
	while (periph->periph_active != 0) {
		periph->periph_flags |= PERIPH_WAITDRAIN;
		cv_wait(periph_cv_active(periph), chan_mtx(chan));
	}
	mutex_exit(chan_mtx(chan));
}

/*
 * scsipi_kill_pending:
 *
 *	Kill off all pending xfers for a periph.
 *
 *	NOTE: Must be called with channel lock held
 */
void
scsipi_kill_pending(struct scsipi_periph *periph)
{
	struct scsipi_channel *chan = periph->periph_channel;

	(*chan->chan_bustype->bustype_kill_pending)(periph);
	while (periph->periph_active != 0) {
		periph->periph_flags |= PERIPH_WAITDRAIN;
		cv_wait(periph_cv_active(periph), chan_mtx(chan));
	}
}

/*
 * scsipi_print_cdb:
 * prints a command descriptor block (for debug purpose, error messages,
 * SCSIVERBOSE, ...)
 */
void
scsipi_print_cdb(struct scsipi_generic *cmd)
{
	int i, j;

 	printf("0x%02x", cmd->opcode);

 	switch (CDB_GROUPID(cmd->opcode)) {
 	case CDB_GROUPID_0:
 		j = CDB_GROUP0;
 		break;
 	case CDB_GROUPID_1:
 		j = CDB_GROUP1;
 		break;
 	case CDB_GROUPID_2:
 		j = CDB_GROUP2;
 		break;
 	case CDB_GROUPID_3:
 		j = CDB_GROUP3;
 		break;
 	case CDB_GROUPID_4:
 		j = CDB_GROUP4;
 		break;
 	case CDB_GROUPID_5:
 		j = CDB_GROUP5;
 		break;
 	case CDB_GROUPID_6:
 		j = CDB_GROUP6;
 		break;
 	case CDB_GROUPID_7:
 		j = CDB_GROUP7;
 		break;
 	default:
 		j = 0;
 	}
 	if (j == 0)
 		j = sizeof (cmd->bytes);
 	for (i = 0; i < j-1; i++) /* already done the opcode */
 		printf(" %02x", cmd->bytes[i]);
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
        printf("scsipi_interpret_sense: Got sense for command 0x%02x\n", xs->cmd->opcode);
        key = SSD_SENSE_KEY(sense->flags);
        printf("  Sense Key: 0x%x, ASC: 0x%02x, ASCQ: 0x%02x\n", key, sense->asc, sense->ascq);
        printf("  Full Sense Data:");
        for (int i = 0; i < (ssize_t)sizeof(struct scsi_sense_data); i++) {
                printf(" %02x", ((uint8_t *)sense)[i]);
        }
        printf("\n");

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

#ifndef PORT_AMIGA
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

#ifndef PORT_AMIGA
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
		for (i = 0; i < (ssize_t)sizeof (sense); i++)
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
#ifdef PORT_AMIGA
	    retries, 5000, NULL, flags);
#else
	    retries, 10000, NULL, flags);
#endif
}

#ifndef PORT_AMIGA
static const struct scsipi_inquiry3_pattern {
	const char vendor[8];
	const char product[16];
	const char revision[4];
} scsipi_inquiry3_quirk[] = {
	{ "ES-6600 ", "", "" },
};
#endif

static int
scsipi_inquiry3_ok(const struct scsipi_inquiry_data *ib)
{
#ifndef PORT_AMIGA
	for (size_t i = 0; i < __arraycount(scsipi_inquiry3_quirk); i++) {
		const struct scsipi_inquiry3_pattern *q =
		    &scsipi_inquiry3_quirk[i];
#define MATCH(field) \
    (q->field[0] ? memcmp(ib->field, q->field, sizeof(ib->field)) == 0 : 1)
		if (MATCH(vendor) && MATCH(product) && MATCH(revision))
			return 0;
	}
#else
	(void)ib;
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
#ifdef PORT_AMIGA
	    3000, NULL, flags | XS_CTL_DATA_IN);
#else
	    10000, NULL, flags | XS_CTL_DATA_IN);
#endif
	if (!error &&
	    inqbuf->additional_length > SCSIPI_INQUIRY_LENGTH_SCSI2 - 4) {
	    if (scsipi_inquiry3_ok(inqbuf)) {
#if 0
printf("inquire: addlen=%d, retrying\n", inqbuf->additional_length);
#endif
		cmd.length = SCSIPI_INQUIRY_LENGTH_SCSI3;
		error = scsipi_command(periph, (void *)&cmd, sizeof(cmd),
		    (void *)inqbuf, SCSIPI_INQUIRY_LENGTH_SCSI3, retries,
#ifdef PORT_AMIGA
		    1000, NULL, flags | XS_CTL_DATA_IN);
#else
		    10000, NULL, flags | XS_CTL_DATA_IN);
#endif
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
		memcpy(inqbuf->vendor, "ADAPTEC ACB-4000            ", 28);
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
		memcpy(inqbuf->vendor, "EMULEX  MT-02 QIC           ", 28);
	}
#endif /* SCSI_OLD_NOINQUIRY */

	return error;
}

#ifndef PORT_AMIGA
/*
 * scsipi_prevent:
 *
 *	Prevent or allow the user to remove the media
 */
int
scsipi_prevent(struct scsipi_periph *periph, int type, int flags)
{
	struct scsi_prevent_allow_medium_removal cmd;

	if (periph->periph_quirks & PQUIRK_NODOORLOCK)
		return 0;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = SCSI_PREVENT_ALLOW_MEDIUM_REMOVAL;
	cmd.how = type;

	return (scsipi_command(periph, (void *)&cmd, sizeof(cmd), 0, 0,
	    SCSIPIRETRIES, 5000, NULL, flags));
}

/*
 * scsipi_start:
 *
 *	Send a START UNIT.
 */
int
scsipi_start(struct scsipi_periph *periph, int type, int flags)
{
	struct scsipi_start_stop cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = START_STOP;
	cmd.byte2 = 0x00;
	cmd.how = type;

	return scsipi_command(periph, (void *)&cmd, sizeof(cmd), 0, 0,
	    SCSIPIRETRIES, (type & SSS_START) ? 60000 : 10000, NULL, flags);
}
#endif

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

	return scsipi_command(periph, (void *)&cmd, sizeof(cmd),
	    (void *)data, len, retries, timeout, NULL, flags | XS_CTL_DATA_IN);
}

#ifndef PORT_AMIGA
int
scsipi_mode_sense_big(struct scsipi_periph *periph, int byte2, int page,
    struct scsi_mode_parameter_header_10 *data, int len, int flags, int retries,
    int timeout)
{
	struct scsi_mode_sense_10 cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = SCSI_MODE_SENSE_10;
	cmd.byte2 = byte2;
	cmd.page = page;
	_lto2b(len, cmd.length);

	return scsipi_command(periph, (void *)&cmd, sizeof(cmd),
	    (void *)data, len, retries, timeout, NULL, flags | XS_CTL_DATA_IN);
}

int
scsipi_mode_select(struct scsipi_periph *periph, int byte2,
    struct scsi_mode_parameter_header_6 *data, int len, int flags, int retries,
    int timeout)
{
	struct scsi_mode_select_6 cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = SCSI_MODE_SELECT_6;
	cmd.byte2 = byte2;
	cmd.length = len & 0xff;

	return scsipi_command(periph, (void *)&cmd, sizeof(cmd),
	    (void *)data, len, retries, timeout, NULL, flags | XS_CTL_DATA_OUT);
}

int
scsipi_mode_select_big(struct scsipi_periph *periph, int byte2,
    struct scsi_mode_parameter_header_10 *data, int len, int flags, int retries,
    int timeout)
{
	struct scsi_mode_select_10 cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = SCSI_MODE_SELECT_10;
	cmd.byte2 = byte2;
	_lto2b(len, cmd.length);

	return scsipi_command(periph, (void *)&cmd, sizeof(cmd),
	    (void *)data, len, retries, timeout, NULL, flags | XS_CTL_DATA_OUT);
}

/*
 * scsipi_get_opcodeinfo:
 *
 * query the device for supported commands and their timeout
 * building a timeout lookup table if timeout information is available.
 */
void
scsipi_get_opcodeinfo(struct scsipi_periph *periph)
{
	u_int8_t *data;
	int len = 16*1024;
	int rc;
	struct scsi_repsuppopcode cmd;

	/* refrain from asking for supported opcodes */
	if (periph->periph_quirks & PQUIRK_NOREPSUPPOPC ||
	    periph->periph_type == T_PROCESSOR || /* spec. */
	    periph->periph_type == T_CDROM) /* spec. */
		return;

	scsipi_free_opcodeinfo(periph);

	/*
	 * query REPORT SUPPORTED OPERATION CODES
	 * if OK
	 *   enumerate all codes
	 *     if timeout exists insert maximum into opcode table
	 */

	data = malloc(len, M_DEVBUF, M_WAITOK|M_ZERO);

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = SCSI_MAINTENANCE_IN;
	cmd.svcaction = RSOC_REPORT_SUPPORTED_OPCODES;
	cmd.repoption = RSOC_RCTD|RSOC_ALL;
	_lto4b(len, cmd.alloclen);

	rc = scsipi_command(periph, (void *)&cmd, sizeof(cmd),
			    (void *)data, len, 0, 1000, NULL,
			    XS_CTL_DATA_IN|XS_CTL_SILENT);

	if (rc == 0) {
		int count;
                int dlen = _4btol(data);
                u_int8_t *c = data + 4;

		SC_DEBUG(periph, SCSIPI_DB3,
			 ("supported opcode timeout-values loaded\n"));
		SC_DEBUG(periph, SCSIPI_DB3,
			 ("CMD  LEN  SA    spec  nom. time  cmd timeout\n"));

		struct scsipi_opcodes *tot = malloc(sizeof(struct scsipi_opcodes),
		    M_DEVBUF, M_WAITOK|M_ZERO);

		count = 0;
                while (tot != NULL &&
		       dlen >= (int)sizeof(struct scsi_repsupopcode_all_commands_descriptor)) {
                        struct scsi_repsupopcode_all_commands_descriptor *acd
				= (struct scsi_repsupopcode_all_commands_descriptor *)c;
#ifdef SCSIPI_DEBUG
                        int cdblen = _2btol((const u_int8_t *)&acd->cdblen);
#endif
                        dlen -= sizeof(struct scsi_repsupopcode_all_commands_descriptor);
                        c += sizeof(struct scsi_repsupopcode_all_commands_descriptor);
                        SC_DEBUG(periph, SCSIPI_DB3,
				 ("0x%02x(%2d) ", acd->opcode, cdblen));

			tot->opcode_info[acd->opcode].ti_flags = SCSIPI_TI_VALID;

                        if (acd->flags & RSOC_ACD_SERVACTV) {
                                SC_DEBUGN(periph, SCSIPI_DB3,
					 ("0x%02x%02x ",
					  acd->serviceaction[0],
					  acd->serviceaction[1]));
                        } else {
				SC_DEBUGN(periph, SCSIPI_DB3, ("       "));
                        }

                        if (acd->flags & RSOC_ACD_CTDP
			    && dlen >= (int)sizeof(struct scsi_repsupopcode_timeouts_descriptor)) {
                                struct scsi_repsupopcode_timeouts_descriptor *td
					= (struct scsi_repsupopcode_timeouts_descriptor *)c;
                                long nomto = _4btol(td->nom_process_timeout);
                                long cmdto = _4btol(td->cmd_process_timeout);
				long t = (cmdto > nomto) ? cmdto : nomto;

                                dlen -= sizeof(struct scsi_repsupopcode_timeouts_descriptor);
                                c += sizeof(struct scsi_repsupopcode_timeouts_descriptor);

                                SC_DEBUGN(periph, SCSIPI_DB3,
					  ("0x%02x %10ld %10ld",
					   td->cmd_specific,
					   nomto, cmdto));

				if (t > tot->opcode_info[acd->opcode].ti_timeout) {
					tot->opcode_info[acd->opcode].ti_timeout = t;
					++count;
				}
                        }
                        SC_DEBUGN(periph, SCSIPI_DB3,("\n"));
                }

		if (count > 0) {
			periph->periph_opcs = tot;
		} else {
			free(tot, M_DEVBUF);
			SC_DEBUG(periph, SCSIPI_DB3,
			 	("no usable timeout values available\n"));
		}
	} else {
		SC_DEBUG(periph, SCSIPI_DB3,
			 ("SCSI_MAINTENANCE_IN"
			  "[RSOC_REPORT_SUPPORTED_OPCODES] failed error=%d"
			  " - no device provided timeout "
			  "values available\n", rc));
	}

	free(data, M_DEVBUF);
}

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
 * scsipi_free_opcodeinfo:
 *
 * free the opcode information table
 */
void
scsipi_free_opcodeinfo(struct scsipi_periph *periph)
{
	if (periph->periph_opcs != NULL) {
		free(periph->periph_opcs, M_DEVBUF);
	}

	periph->periph_opcs = NULL;
}
#endif  /* !PORT_AMIGA */

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

	mutex_enter(chan_mtx(chan));
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
		mutex_exit(chan_mtx(chan));
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
#ifndef PORT_AMIGA
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
			mutex_exit(chan_mtx(chan));
			return;
		}
		cv_broadcast(xs_cv(xs));
		mutex_exit(chan_mtx(chan));
		goto out;
	}

	/*
	 * Catch the extremely common case of I/O completing
	 * without error; no use in taking a context switch
	 * if we can handle it in interrupt context.
	 */
	if (xs->error == XS_NOERROR) {
		mutex_exit(chan_mtx(chan));
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
	mutex_exit(chan_mtx(chan));
#endif

 out:
	/*
	 * If there are more xfers on the channel's queue, attempt to
	 * run them.
	 */
	scsipi_run_queue(chan);
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
#ifndef PORT_AMIGA
			scsipi_periph_thaw_locked(periph, 1);
#endif
			mutex_exit(chan_mtx(chan));
			if (xs->resid < xs->datalen) {
				printf("we read %d bytes of sense anyway:\n",
				    xs->datalen - xs->resid);
#ifndef PORT_AMIGA
				scsipi_print_sense_data((void *)xs->data, 0);
#endif
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

#ifndef PORT_AMIGA
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
#ifndef PORT_AMIGA
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
#ifdef PORT_AMIGA
                error = scsipi_interpret_sense(xs);
#else
		error = (*chan->chan_bustype->bustype_interpret_sense)(xs);
#endif
		break;

	case XS_RESOURCE_SHORTAGE:
		/*
		 * XXX Should freeze channel's queue.
		 */
		scsipi_printaddr(periph);
		printf("adapter resource shortage\n");
		/* FALLTHROUGH */

	case XS_BUSY:
#ifndef PORT_AMIGA
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
#else  /* PORT_AMIGA */
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
#ifdef PORT_AMIGA
                printf("restart %p retries left=%d\n", xs, xs->xs_retries);
#endif
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
#ifndef PORT_AMIGA
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
#ifndef PORT_AMIGA
	if (xs->error != XS_NOERROR)
		scsipi_periph_thaw_locked(periph, 1);
	mutex_exit(chan_mtx(chan));

	if (periph->periph_switch->psw_done)
		periph->periph_switch->psw_done(xs, error);
#endif

#ifdef PORT_AMIGA
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
#endif

	mutex_enter(chan_mtx(chan));
	if (xs->xs_control & XS_CTL_ASYNC)
		scsipi_put_xs(xs);
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
#ifdef PORT_AMIGA
                printf("bus reset interrupted request sense\n");
#endif
		return;
	case EIO:
		 /* request sense couldn't be performed */
		/*
		 * XXX this isn't quite right but we don't have anything
		 * better for now
		 */
		xs->error = XS_DRIVER_STUFFUP;
#ifdef PORT_AMIGA
                printf("request sense driver stuffup\n");
#endif
		return;
	default:
		 /* Notify that request sense failed. */
		xs->error = XS_DRIVER_STUFFUP;
		scsipi_printaddr(periph);
		printf("request sense failed with error %d\n", error);
		return;
	}
}

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
#ifndef PORT_AMIGA
	if (xs->xs_control & XS_CTL_THAW_PERIPH)
		scsipi_periph_thaw_locked(xs->xs_periph, 1);
#endif
	return 0;
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

	SDT_PROBE1(scsi, base, queue, batch__start,  chan);
	for (;;) {
		mutex_enter(chan_mtx(chan));
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
			mutex_exit(chan_mtx(chan));
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
		mutex_exit(chan_mtx(chan));
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
				mutex_exit(chan_mtx(chan));

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
		mutex_exit(chan_mtx(chan));

		SDT_PROBE2(scsi, base, queue, run,  chan, xs);
		scsipi_adapter_request(chan, ADAPTER_REQ_RUN_XFER, xs);
	}
	SDT_PROBE1(scsi, base, queue, batch__done,  chan);
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
#ifdef DEBUG
        if (((uint) xs > 0x10000000) || ((uint) xs < 0x02000000)) {
            panic("Invalid xs %08lx", (uint) xs);
            return (1);
        }
#endif
        /*
         * Set the LUN in the CDB if we have an older device. We also
         * set it for more modern SCSI-2 devices "just in case".
         *
         * For Amiga, this code is taken from the scsi_base bustype_cmd
         * implementation, and is placed here to avoid a call through
         * indirection.
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
			panic("Invalid tag mask %lx for %08lx cmd %02lx",
                              XS_CTL_TAGTYPE(xs), xs,
                              (xs->cmd != NULL) ? xs->cmd->opcode : 0xffffffff);
			xs->xs_tag_type = MSG_SIMPLE_Q_TAG;
			break;
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
#ifdef PORT_AMIGA
	oasync = (xs->xs_control & XS_CTL_ASYNC);
	poll = (xs->xs_control & XS_CTL_POLL);
	if (poll != 0)
		xs->xs_control &= ~XS_CTL_ASYNC;
	async = (xs->xs_control & XS_CTL_ASYNC);
#else /* !PORT_AMIGA */
	oasync =  (xs->xs_control & XS_CTL_ASYNC);
	if (chan->chan_thread == NULL || (xs->xs_control & XS_CTL_POLL) != 0)
		xs->xs_control &= ~XS_CTL_ASYNC;

	async = (xs->xs_control & XS_CTL_ASYNC);
	poll = (xs->xs_control & XS_CTL_POLL);
#endif /* !PORT_AMIGA */

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
#ifndef PORT_AMIGA
                /*
                 * Eliminated for Amiga as this should not happen because:
                 *      poll = xs->xs_control & XS_CTL_POLL
                 * and the called function can only return non-zero in
                 * the case where:
                 *      (xs->xs_control & XS_CTL_POLL) != 0
                 */
		if (poll == 0) {
			scsipi_printaddr(periph);
			printf("not polling, but enqueue failed with %d\n",
			    error);
			panic("scsipi_execute_xs");
		}
#endif

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

#ifdef PORT_AMIGA
void scsipi_completion_poll(struct scsipi_channel *chan);

/*
 * scsipi_completion_poll
 * ----------------------
 * Finish completions or perform other channel thread service operations.
 */
void
scsipi_completion_poll(struct scsipi_channel *chan)
{
    struct scsipi_xfer *xs = NULL;

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

#else /* !PORT_AMIGA */

/*
 * scsipi_completion_thread:
 *
 *	This is the completion thread.  We wait for errors on
 *	asynchronous xfers, and perform the error handling
 *	function, restarting the command, if necessary.
 */
static void
scsipi_completion_thread(void *arg)
{
	struct scsipi_channel *chan = arg;
	struct scsipi_xfer *xs;

	if (chan->chan_init_cb)
		(*chan->chan_init_cb)(chan, chan->chan_init_cb_arg);

	mutex_enter(chan_mtx(chan));
	chan->chan_flags |= SCSIPI_CHAN_TACTIVE;
	for (;;) {
		xs = TAILQ_FIRST(&chan->chan_complete);
		if (xs == NULL && chan->chan_tflags == 0) {
			/* nothing to do; wait */
			cv_wait(chan_cv_complete(chan), chan_mtx(chan));
			continue;
		}
		if (chan->chan_tflags & SCSIPI_CHANT_CALLBACK) {
			/* call chan_callback from thread context */
			chan->chan_tflags &= ~SCSIPI_CHANT_CALLBACK;
			chan->chan_callback(chan, chan->chan_callback_arg);
			continue;
		}
		if (chan->chan_tflags & SCSIPI_CHANT_GROWRES) {
			/* attempt to get more openings for this channel */
			chan->chan_tflags &= ~SCSIPI_CHANT_GROWRES;
			mutex_exit(chan_mtx(chan));
			scsipi_adapter_request(chan,
			    ADAPTER_REQ_GROW_RESOURCES, NULL);
			scsipi_channel_thaw(chan, 1);
			if (chan->chan_tflags & SCSIPI_CHANT_GROWRES)
				kpause("scsizzz", FALSE, hz/10, NULL);
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
		if (chan->chan_tflags & SCSIPI_CHANT_SHUTDOWN) {
			break;
		}
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

	chan->chan_thread = NULL;

	/* In case parent is waiting for us to exit. */
	cv_broadcast(chan_cv_thread(chan));
	mutex_exit(chan_mtx(chan));

	kthread_exit(0);
}

/*
 * scsipi_thread_call_callback:
 *
 * 	request to call a callback from the completion thread
 */
int
scsipi_thread_call_callback(struct scsipi_channel *chan,
    void (*callback)(struct scsipi_channel *, void *), void *arg)
{

	mutex_enter(chan_mtx(chan));
	if ((chan->chan_flags & SCSIPI_CHAN_TACTIVE) == 0) {
		/* kernel thread doesn't exist yet */
		mutex_exit(chan_mtx(chan));
		return ESRCH;
	}
	if (chan->chan_tflags & SCSIPI_CHANT_CALLBACK) {
		mutex_exit(chan_mtx(chan));
		return EBUSY;
	}
	scsipi_channel_freeze(chan, 1);
	chan->chan_callback = callback;
	chan->chan_callback_arg = arg;
	chan->chan_tflags |= SCSIPI_CHANT_CALLBACK;
	cv_broadcast(chan_cv_complete(chan));
	mutex_exit(chan_mtx(chan));
	return 0;
}

/*
 * scsipi_async_event:
 *
 *	Handle an asynchronous event from an adapter.
 */
void
scsipi_async_event(struct scsipi_channel *chan, scsipi_async_event_t event,
    void *arg)
{
	bool lock = chan_running(chan) > 0;

	if (lock)
		mutex_enter(chan_mtx(chan));
	switch (event) {
	case ASYNC_EVENT_MAX_OPENINGS:
		scsipi_async_event_max_openings(chan,
		    (struct scsipi_max_openings *)arg);
		break;

	case ASYNC_EVENT_XFER_MODE:
		if (chan->chan_bustype->bustype_async_event_xfer_mode) {
			chan->chan_bustype->bustype_async_event_xfer_mode(
			    chan, arg);
		}
		break;
	case ASYNC_EVENT_RESET:
		scsipi_async_event_channel_reset(chan);
		break;
	}
	if (lock)
		mutex_exit(chan_mtx(chan));
}

/*
 * scsipi_async_event_max_openings:
 *
 *	Update the maximum number of outstanding commands a
 *	device may have.
 */
static void
scsipi_async_event_max_openings(struct scsipi_channel *chan,
    struct scsipi_max_openings *mo)
{
	struct scsipi_periph *periph;
	int minlun, maxlun;

	if (mo->mo_lun == -1) {
		/*
		 * Wildcarded; apply it to all LUNs.
		 */
		minlun = 0;
		maxlun = chan->chan_nluns - 1;
	} else
		minlun = maxlun = mo->mo_lun;

	/* XXX This could really suck with a large LUN space. */
	for (; minlun <= maxlun; minlun++) {
		periph = scsipi_lookup_periph_locked(chan, mo->mo_target, minlun);
		if (periph == NULL)
			continue;

		if (mo->mo_openings < periph->periph_openings)
			periph->periph_openings = mo->mo_openings;
		else if (mo->mo_openings > periph->periph_openings &&
		    (periph->periph_flags & PERIPH_GROW_OPENINGS) != 0)
			periph->periph_openings = mo->mo_openings;
	}
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

/*
 * scsipi_channel_reset:
 *
 *	handle scsi bus reset
 * called with channel lock held
 */
static void
scsipi_async_event_channel_reset(struct scsipi_channel *chan)
{
	struct scsipi_xfer *xs, *xs_next;
	struct scsipi_periph *periph;
	int target, lun;

	/*
	 * Channel has been reset. Also mark as reset pending REQUEST_SENSE
	 * commands; as the sense is not available any more.
	 * can't call scsipi_done() from here, as the command has not been
	 * sent to the adapter yet (this would corrupt accounting).
	 */

	for (xs = TAILQ_FIRST(&chan->chan_queue); xs != NULL; xs = xs_next) {
		xs_next = TAILQ_NEXT(xs, channel_q);
		if (xs->xs_control & XS_CTL_REQSENSE) {
			TAILQ_REMOVE(&chan->chan_queue, xs, channel_q);
			xs->error = XS_RESET;
			if ((xs->xs_control & XS_CTL_ASYNC) != 0)
				TAILQ_INSERT_TAIL(&chan->chan_complete, xs,
				    channel_q);
		}
	}
	cv_broadcast(chan_cv_complete(chan));
	/* Catch xs with pending sense which may not have a REQSENSE xs yet */
	for (target = 0; target < chan->chan_ntargets; target++) {
		if (target == chan->chan_id)
			continue;
		for (lun = 0; lun <  chan->chan_nluns; lun++) {
			periph = scsipi_lookup_periph_locked(chan, target, lun);
			if (periph) {
				xs = periph->periph_xscheck;
				if (xs)
					xs->error = XS_RESET;
			}
		}
	}
}

/*
 * scsipi_target_detach:
 *
 *	detach all periph associated with a I_T
 * 	must be called from valid thread context
 */
int
scsipi_target_detach(struct scsipi_channel *chan, int target, int lun,
    int flags)
{
	struct scsipi_periph *periph;
	device_t tdev;
	int ctarget, mintarget, maxtarget;
	int clun, minlun, maxlun;
	int error = 0;

	if (target == -1) {
		mintarget = 0;
		maxtarget = chan->chan_ntargets;
	} else {
		if (target == chan->chan_id)
			return EINVAL;
		if (target < 0 || target >= chan->chan_ntargets)
			return EINVAL;
		mintarget = target;
		maxtarget = target + 1;
	}

	if (lun == -1) {
		minlun = 0;
		maxlun = chan->chan_nluns;
	} else {
		if (lun < 0 || lun >= chan->chan_nluns)
			return EINVAL;
		minlun = lun;
		maxlun = lun + 1;
	}

	/* for config_detach */
	KERNEL_LOCK(1, curlwp);

	mutex_enter(chan_mtx(chan));
	for (ctarget = mintarget; ctarget < maxtarget; ctarget++) {
		if (ctarget == chan->chan_id)
			continue;

		for (clun = minlun; clun < maxlun; clun++) {
			periph = scsipi_lookup_periph_locked(chan, ctarget, clun);
			if (periph == NULL)
				continue;
			tdev = periph->periph_dev;
			mutex_exit(chan_mtx(chan));
			error = config_detach(tdev, flags);
			if (error)
				goto out;
			mutex_enter(chan_mtx(chan));
			KASSERT(scsipi_lookup_periph_locked(chan, ctarget, clun) == NULL);
		}
	}
	mutex_exit(chan_mtx(chan));

out:
	KERNEL_UNLOCK_ONE(curlwp);

	return error;
}

/*
 * scsipi_adapter_addref:
 *
 *	Add a reference to the adapter pointed to by the provided
 *	link, enabling the adapter if necessary.
 */
int
scsipi_adapter_addref(struct scsipi_adapter *adapt)
{
	int error = 0;

	if (atomic_inc_uint_nv(&adapt->adapt_refcnt) == 1
	    && adapt->adapt_enable != NULL) {
		scsipi_adapter_lock(adapt);
		error = scsipi_adapter_enable(adapt, 1);
		scsipi_adapter_unlock(adapt);
		if (error)
			atomic_dec_uint(&adapt->adapt_refcnt);
	}
	return error;
}

/*
 * scsipi_adapter_delref:
 *
 *	Delete a reference to the adapter pointed to by the provided
 *	link, disabling the adapter if possible.
 */
void
scsipi_adapter_delref(struct scsipi_adapter *adapt)
{
	membar_release();
	if (atomic_dec_uint_nv(&adapt->adapt_refcnt) == 0
	    && adapt->adapt_enable != NULL) {
		membar_acquire();
		scsipi_adapter_lock(adapt);
		(void) scsipi_adapter_enable(adapt, 0);
		scsipi_adapter_unlock(adapt);
	}
}

static struct scsipi_syncparam {
	int	ss_factor;
	int	ss_period;	/* ns * 100 */
} scsipi_syncparams[] = {
	{ 0x08,		 625 },	/* FAST-160 (Ultra320) */
	{ 0x09,		1250 },	/* FAST-80 (Ultra160) */
	{ 0x0a,		2500 },	/* FAST-40 40MHz (Ultra2) */
	{ 0x0b,		3030 },	/* FAST-40 33MHz (Ultra2) */
	{ 0x0c,		5000 },	/* FAST-20 (Ultra) */
};
static const int scsipi_nsyncparams =
    sizeof(scsipi_syncparams) / sizeof(scsipi_syncparams[0]);

int
scsipi_sync_period_to_factor(int period /* ns * 100 */)
{
	int i;

	for (i = 0; i < scsipi_nsyncparams; i++) {
		if (period <= scsipi_syncparams[i].ss_period)
			return scsipi_syncparams[i].ss_factor;
	}

	return (period / 100) / 4;
}

int
scsipi_sync_factor_to_period(int factor)
{
	int i;

	for (i = 0; i < scsipi_nsyncparams; i++) {
		if (factor == scsipi_syncparams[i].ss_factor)
			return scsipi_syncparams[i].ss_period;
	}

	return (factor * 4) * 100;
}

int
scsipi_sync_factor_to_freq(int factor)
{
	int i;

	for (i = 0; i < scsipi_nsyncparams; i++) {
		if (factor == scsipi_syncparams[i].ss_factor)
			return 100000000 / scsipi_syncparams[i].ss_period;
	}

	return 10000000 / ((factor * 4) * 10);
}

static inline void
scsipi_adapter_lock(struct scsipi_adapter *adapt)
{

	if ((adapt->adapt_flags & SCSIPI_ADAPT_MPSAFE) == 0)
		KERNEL_LOCK(1, NULL);
}

static inline void
scsipi_adapter_unlock(struct scsipi_adapter *adapt)
{

	if ((adapt->adapt_flags & SCSIPI_ADAPT_MPSAFE) == 0)
		KERNEL_UNLOCK_ONE(NULL);
}

void
scsipi_adapter_minphys(struct scsipi_channel *chan, struct buf *bp)
{
	struct scsipi_adapter *adapt = chan->chan_adapter;

	scsipi_adapter_lock(adapt);
	(adapt->adapt_minphys)(bp);
	scsipi_adapter_unlock(chan->chan_adapter);
}
#endif /* !PORT_AMIGA */

#ifdef PORT_AMIGA
static
#endif
void
scsipi_adapter_request(struct scsipi_channel *chan,
	scsipi_adapter_req_t req, void *arg)

{
	struct scsipi_adapter *adapt = chan->chan_adapter;

#ifndef PORT_AMIGA
	scsipi_adapter_lock(adapt);
#endif
	SDT_PROBE3(scsi, base, adapter, request__start,  chan, req, arg);
	(adapt->adapt_request)(chan, req, arg);
	SDT_PROBE3(scsi, base, adapter, request__done,  chan, req, arg);
#ifndef PORT_AMIGA
	scsipi_adapter_unlock(adapt);
#endif
}

#ifndef PORT_AMIGA
int
scsipi_adapter_ioctl(struct scsipi_channel *chan, u_long cmd,
	void *data, int flag, struct proc *p)
{
	struct scsipi_adapter *adapt = chan->chan_adapter;
	int error;

	if (adapt->adapt_ioctl == NULL)
		return ENOTTY;

	scsipi_adapter_lock(adapt);
	error = (adapt->adapt_ioctl)(chan, cmd, data, flag, p);
	scsipi_adapter_unlock(adapt);
	return error;
}

int
scsipi_adapter_enable(struct scsipi_adapter *adapt, int enable)
{
	int error;

	scsipi_adapter_lock(adapt);
	error = (adapt->adapt_enable)(adapt->adapt_dev, enable);
	scsipi_adapter_unlock(adapt);
	return error;
}
#endif  /* !PORT_AMIGA */

#ifdef SCSIPI_DEBUG
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
			show_mem(xs->data, uimin(64, xs->datalen));
	} else
		printf("-RESET-\n");
}

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
#endif /* SCSIPI_DEBUG */
