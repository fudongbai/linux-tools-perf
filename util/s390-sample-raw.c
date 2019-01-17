// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright IBM Corp. 2019
 * Author(s): Thomas Richter <tmricht@linux.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2 only)
 * as published by the Free Software Foundation.
 *
 * Architecture specific trace_event function. Save event's bc000 raw data
 * to file. File name is aux.ctr.## where ## stands for the CPU number the
 * sample was taken from.
 */

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <sys/stat.h>
#include <linux/compiler.h>
#include <asm/byteorder.h>

#include "debug.h"
#include "util.h"
#include "auxtrace.h"
#include "session.h"
#include "evlist.h"
#include "config.h"
#include "color.h"
#include "sample-raw.h"
#include "s390-cpumcf-kernel.h"

static size_t ctrset_size(struct cf_ctrset_entry *set)
{
	return sizeof(*set) + set->ctr * sizeof(u64);
}

static bool ctrset_valid(struct cf_ctrset_entry *set)
{
	return set->def == S390_CPUMCF_DIAG_DEF;
}

/* CPU Measurement Counter Facility raw data is a byte stream. It is 8 byte
 * aligned and might have trailing padding bytes.
 * Display the raw data on screen.
 */
static bool s390_cpumcfdg_testctr(struct perf_sample *sample)
{
	size_t len = sample->raw_size, offset = 0;
	unsigned char *buf = sample->raw_data;
	struct cf_trailer_entry *te;
	struct cf_ctrset_entry *cep, ce;

	if (!len)
		return false;
	while (offset < len) {
		cep = (struct cf_ctrset_entry *)(buf + offset);
		ce.def = be16_to_cpu(cep->def);
		ce.set = be16_to_cpu(cep->set);
		ce.ctr = be16_to_cpu(cep->ctr);
		ce.res1 = be16_to_cpu(cep->res1);

		if (!ctrset_valid(&ce) || offset + ctrset_size(&ce) > len) {
			/* Raw data for counter sets are always multiple of 8
			 * bytes. Prepending a 4 bytes size field to the
			 * raw data block in the sample causes the perf tool
			 * to append 4 padding bytes to make the raw data part
			 * of the sample a multiple of eight bytes again.
			 *
			 * If the last entry (trailer) is 4 bytes off the raw
			 * area data end, all is good.
			 */
			if (len - offset - sizeof(*te) == 4)
				break;
			pr_err("Invalid counter set entry at %zd\n", offset);
			return false;
		}
		offset += ctrset_size(&ce);
	}
	return true;
}

/* Dump event bc000 on screen, already tested on correctness. */
static void s390_cpumcfdg_dumptrail(const char *color, size_t offset,
				    struct cf_trailer_entry *tep)
{
	struct cf_trailer_entry  te;

	te.flags = be64_to_cpu(tep->flags);
	te.cfvn = be16_to_cpu(tep->cfvn);
	te.csvn = be16_to_cpu(tep->csvn);
	te.cpu_speed = be32_to_cpu(tep->cpu_speed);
	te.timestamp = be64_to_cpu(tep->timestamp);
	te.progusage1 = be64_to_cpu(tep->progusage1);
	te.progusage2 = be64_to_cpu(tep->progusage2);
	te.progusage3 = be64_to_cpu(tep->progusage3);
	te.tod_base = be64_to_cpu(tep->tod_base);
	te.mach_type = be16_to_cpu(tep->mach_type);
	te.res1 = be16_to_cpu(tep->res1);
	te.res2 = be32_to_cpu(tep->res2);

	color_fprintf(stdout, color, "    [%#08zx] Trailer:%c%c%c%c%c"
		      " Cfvn:%d Csvn:%d Speed:%d TOD:%#llx\n",
		      offset, te.clock_base ? 'T' : ' ',
		      te.speed ? 'S' : ' ', te.mtda ? 'M' : ' ',
		      te.caca ? 'C' : ' ', te.lcda ? 'L' : ' ',
		      te.cfvn, te.csvn, te.cpu_speed, te.timestamp);
	color_fprintf(stdout, color, "\t\t1:%lx 2:%lx 3:%lx TOD-Base:%#llx"
		      " Type:%x\n\n",
		      te.progusage1, te.progusage2, te.progusage3,
		      te.tod_base, te.mach_type);
}

static void s390_cpumcfdg_dump(struct perf_sample *sample)
{
	size_t i, len = sample->raw_size, offset = 0;
	unsigned char *buf = sample->raw_data;
	const char *color = PERF_COLOR_BLUE;
	struct cf_ctrset_entry *cep, ce;
	u64 *p;

	while (offset < len) {
		cep = (struct cf_ctrset_entry *)(buf + offset);

		ce.def = be16_to_cpu(cep->def);
		ce.set = be16_to_cpu(cep->set);
		ce.ctr = be16_to_cpu(cep->ctr);
		ce.res1 = be16_to_cpu(cep->res1);

		if (!ctrset_valid(&ce)) {	/* Print trailer */
			s390_cpumcfdg_dumptrail(color, offset,
						(struct cf_trailer_entry *)cep);
			return;
		}

		color_fprintf(stdout, color, "    [%#08zx] Counterset:%d"
			      " Counters:%d\n", offset, ce.set, ce.ctr);
		for (i = 0, p = (u64 *)(cep + 1); i < ce.ctr; i += 2, p += 2)
			color_fprintf(stdout, color,
				      "\tCounter:%03d Value:%#018lx"
				      " Counter:%03d Value:%#018lx\n",
				      i, be64_to_cpu(*p),
				      i + 1, be64_to_cpu(*(p + 1)));
		offset += ctrset_size(&ce);
	}
}

/* S390 specific trace event function. Check for PERF_RECORD_SAMPLE events
 * and if the event was triggered by a counter set diagnostic event display
 * its raw data.
 * The function is only invoked when the dump flag -D is set.
 */
void perf_evlist__s390_sample_raw(struct perf_evlist *evlist, union perf_event *event,
				  struct perf_sample *sample)
{
	struct perf_evsel *ev_bc000;

	if (event->header.type != PERF_RECORD_SAMPLE)
		return;

	ev_bc000 = perf_evlist__event2evsel(evlist, event);
	if (ev_bc000 == NULL ||
	    ev_bc000->attr.config != PERF_EVENT_CPUM_CF_DIAG)
		return;

	/* Display raw data on screen */
	if (!s390_cpumcfdg_testctr(sample)) {
		pr_err("Invalid counter set data encountered\n");
		return;
	}
	s390_cpumcfdg_dump(sample);
}
