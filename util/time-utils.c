#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>

#include "perf.h"
#include "debug.h"
#include "time-utils.h"
#include "util.h"

static int parse_timestr_sec_nsec(struct perf_time_interval *ptime,
				  char *start_str, char *end_str)
{
	if (start_str && (*start_str != '\0') &&
	    (parse_nsec_time(start_str, &ptime->start) != 0)) {
		return -1;
	}

	if (end_str && (*end_str != '\0') &&
	    (parse_nsec_time(end_str, &ptime->end) != 0)) {
		return -1;
	}

	return 0;
}

int perf_time__parse_str(struct perf_time_interval *ptime, const char *ostr)
{
	char *start_str, *end_str;
	char *d, *str;
	int rc = 0;

	if (ostr == NULL || *ostr == '\0')
		return 0;

	/* copy original string because we need to modify it */
	str = strdup(ostr);
	if (str == NULL)
		return -ENOMEM;

	ptime->start = 0;
	ptime->end = 0;

	/* str has the format: <start>,<stop>
	 * variations: <start>,
	 *             ,<stop>
	 *             ,
	 */
	start_str = str;
	d = strchr(start_str, ',');
	if (d) {
		*d = '\0';
		++d;
	}
	end_str = d;

	rc = parse_timestr_sec_nsec(ptime, start_str, end_str);

	free(str);

	/* make sure end time is after start time if it was given */
	if (rc == 0 && ptime->end && ptime->end < ptime->start)
		return -EINVAL;

	pr_debug("start time %" PRIu64 ", ", ptime->start);
	pr_debug("end time %" PRIu64 "\n", ptime->end);

	return rc;
}

bool perf_time__skip_sample(struct perf_time_interval *ptime, u64 timestamp)
{
	/* if time is not set don't drop sample */
	if (timestamp == 0)
		return false;

	/* otherwise compare sample time to time window */
	if ((ptime->start && timestamp < ptime->start) ||
	    (ptime->end && timestamp > ptime->end)) {
		return true;
	}

	return false;
}
