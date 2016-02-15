#ifndef __PERF_MEM_EVENTS_H
#define __PERF_MEM_EVENTS_H

#include <stdbool.h>

struct perf_mem_event {
	bool		record;
	const char	*name;
};

enum {
	PERF_MEM_EVENTS__LOAD,
	PERF_MEM_EVENTS__STORE,
	PERF_MEM_EVENTS__MAX,
};

extern struct perf_mem_event perf_mem_events[PERF_MEM_EVENTS__MAX];

#endif /* __PERF_MEM_EVENTS_H */
