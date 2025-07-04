/*	$NetBSD: scsipiconf.c,v 1.45 2019/03/28 10:44:29 kardel Exp $	*/

/*-
 * Copyright (c) 1998, 1999, 2004 The NetBSD Foundation, Inc.
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

/*
 * Originally written by Julian Elischer (julian@tfs.com)
 * for TRW Financial Systems for use under the MACH(2.5) operating system.
 *
 * TRW Financial Systems, in accordance with their agreement with Carnegie
 * Mellon University, makes this software available to CMU to distribute
 * or use in any manner that they see fit as long as this message is kept with
 * the software. For this reason TFS also grants any other persons or
 * organisations permission to use or modify this software.
 *
 * TFS supplies this software to be publicly redistributed
 * on the understanding that TFS is not responsible for the correct
 * functioning of this software in any circumstances.
 *
 * Ported to run under 386BSD by Julian Elischer (julian@tfs.com) Sept 1992
 */

#include <sys/cdefs.h>
#ifdef ENABLE_QUIRKS
#ifdef PORT_AMIGA
#ifdef DEBUG_SCSIPI_BASE
#define SCSIPI_DEBUG
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


#else
__KERNEL_RCSID(0, "$NetBSD: scsipiconf.c,v 1.45 2019/03/28 10:44:29 kardel Exp $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/device.h>
#include <sys/proc.h>

#include <dev/scsipi/scsipi_all.h>
#include <dev/scsipi/scsipiconf.h>
#include <dev/scsipi/scsipi_base.h>

/* Function pointers and stub routines for scsiverbose module */
int (*scsipi_print_sense)(struct scsipi_xfer *, int) = scsipi_print_sense_stub;
void (*scsipi_print_sense_data)(struct scsi_sense_data *, int) =
		scsipi_print_sense_data_stub;

int scsi_verbose_loaded = 0; 

int
scsipi_print_sense_stub(struct scsipi_xfer * xs, int verbosity)
{
	scsipi_load_verbose();
	if (scsi_verbose_loaded)
		return scsipi_print_sense(xs, verbosity);
	else
		return 0;
}

void
scsipi_print_sense_data_stub(struct scsi_sense_data *sense, int verbosity)
{
	scsipi_load_verbose();
	if (scsi_verbose_loaded)
		scsipi_print_sense_data(sense, verbosity);
}

int
scsipi_command(struct scsipi_periph *periph, struct scsipi_generic *cmd,
    int cmdlen, u_char *data_addr, int datalen, int retries, int timeout,
    struct buf *bp, int flags)
{
	struct scsipi_xfer *xs;
	int rc;

	/*
	 * execute unlocked to allow waiting for memory
	 */
	xs = scsipi_make_xs_unlocked(periph, cmd, cmdlen, data_addr, datalen, retries,
	    timeout, bp, flags);
	if (!xs)
		return (ENOMEM);

	mutex_enter(chan_mtx(periph->periph_channel));
	rc = scsipi_execute_xs(xs);
	mutex_exit(chan_mtx(periph->periph_channel));

	return rc;
}

/* 
 * Load the scsiverbose module
 */   
void
scsipi_load_verbose(void)
{
	if (scsi_verbose_loaded == 0)
		module_autoload("scsiverbose", MODULE_CLASS_MISC);
}

/*
 * allocate and init a scsipi_periph structure for a new device.
 */
struct scsipi_periph *
scsipi_alloc_periph(int malloc_flag)
{
	struct scsipi_periph *periph;
	u_int i;

	periph = malloc(sizeof(*periph), M_DEVBUF, malloc_flag|M_ZERO);
	if (periph == NULL)
		return NULL;

	periph->periph_dev = NULL;
	periph->periph_opcs = NULL;

	/*
	 * Start with one command opening.  The periph driver
	 * will grow this if it knows it can take advantage of it.
	 */
	periph->periph_openings = 1;
	periph->periph_active = 0;

	for (i = 0; i < PERIPH_NTAGWORDS; i++)
		periph->periph_freetags[i] = 0xffffffff;

	TAILQ_INIT(&periph->periph_xferq);
	callout_init(&periph->periph_callout, 0);
	cv_init(&periph->periph_cv, "periph");

	return periph;
}

/*
 * cleanup and free scsipi_periph structure
 */
void
scsipi_free_periph(struct scsipi_periph *periph)
{
	scsipi_free_opcodeinfo(periph);
	cv_destroy(&periph->periph_cv);
	free(periph, M_DEVBUF);
}

#endif

/*
 * Return a priority based on how much of the inquiry data matches
 * the patterns for the particular driver.
 */
const void *
scsipi_inqmatch(struct scsipi_inquiry_pattern *inqbuf, const void *base,
    size_t nmatches, size_t matchsize, int *bestpriority)
{
	u_int8_t type;
	const struct scsipi_inquiry_pattern *bestmatch;

	/* Include the qualifier to catch vendor-unique types. */
	type = inqbuf->type;

	for (*bestpriority = 0, bestmatch = 0; nmatches--;
	    base = (const char *)base + matchsize) {
		const struct scsipi_inquiry_pattern *match = base;
		int priority, len;

		if (type != match->type)
			continue;
		if (inqbuf->removable != match->removable)
			continue;
		priority = 2;
		len = strlen(match->vendor);
		if (memcmp(inqbuf->vendor, match->vendor, len))
			continue;
		priority += len;
		len = strlen(match->product);
		if (memcmp(inqbuf->product, match->product, len))
			continue;
		priority += len;
		len = strlen(match->revision);
		if (memcmp(inqbuf->revision, match->revision, len))
			continue;
		priority += len;

#ifdef SCSIPI_DEBUG
		printf("scsipi_inqmatch: %d/%d/%d <%s, %s, %s>\n",
		    priority, match->type, match->removable,
		    match->vendor, match->product, match->revision);
#endif
		if (priority > *bestpriority) {
			*bestpriority = priority;
			bestmatch = base;
		}
	}

	return (bestmatch);
}

#ifndef PORT_AMIGA
const char *
scsipi_dtype(int type)
{
	const char *dtype;

	switch (type) {
	case T_DIRECT:
		dtype = "disk";
		break;
	case T_SEQUENTIAL:
		dtype = "tape";
		break;
	case T_PRINTER:
		dtype = "printer";
		break;
	case T_PROCESSOR:
		dtype = "processor";
		break;
	case T_WORM:
		dtype = "worm";
		break;
	case T_CDROM:
		dtype = "cdrom";
		break;
	case T_SCANNER:
		dtype = "scanner";
		break;
	case T_OPTICAL:
		dtype = "optical";
		break;
	case T_CHANGER:
		dtype = "changer";
		break;
	case T_COMM:
		dtype = "communication";
		break;
	case T_IT8_1:
	case T_IT8_2:
		dtype = "graphic arts pre-press";
		break;
	case T_STORARRAY:
		dtype = "storage array";
		break;
	case T_ENCLOSURE:
		dtype = "enclosure services";
		break;
	case T_SIMPLE_DIRECT:
		dtype = "simplified direct";
		break;
	case T_OPTIC_CARD_RW:
		dtype = "optical card r/w";
		break;
	case T_OBJECT_STORED:
		dtype = "object-based storage";
		break;
	case T_NODEVICE:
		panic("scsipi_dtype: impossible device type");
	default:
		dtype = "unknown";
		break;
	}
	return (dtype);
}
#endif /* !PORT_AMIGA */
#endif /* ENABLE_QUIRKS */
