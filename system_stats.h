#ifndef _SYSTEM_STATS_H_
#define _SYSTEM_STATS_H_

#include "postgres.h"

double SC_CLK_TCK;

#define MINIMUM(a,b)		((a) < (b) ? (a) : (b))
#define FREE(v)				do {if (v != NULL) {pfree(v); v = NULL;}} while(0)
#define S_VALUE(m,n,p)		(((double) ((n) - (m))) / (p) * SC_CLK_TCK)
#define SP_VALUE(m,n,p)		(n < m ? 0 : ((double) ((n) - (m))) / (p) * 100)
#define SP_VALUE_100(m,n,p)	MINIMUM((((double) ((n) - (m))) / (p) * 100), 100.0)

typedef struct {
	float run_1min;
	float run_5min;
	float run_15min;
} load_avg;

typedef struct {
	int memory;
	int ratio;
} overcommit;

typedef struct {
	bool available;
	unsigned long limit;
	unsigned long usage;
	unsigned long rss;
	unsigned long cache;
} cgroup_memory;

typedef struct {
	bool available;
	cgroup_memory cgroup;
	unsigned long total;
	unsigned long free;
	unsigned long buffers;
	unsigned long cached;
	unsigned long dirty;
	unsigned long limit;
	unsigned long as;
	overcommit overcommit;
} meminfo;

typedef struct {
	int fields;
	int cpu_count;
	unsigned long long utime; // (1)
	double utime_diff;
	unsigned long long ntime; // (2)
	double ntime_diff;
	unsigned long long stime; // (3)
	double stime_diff;
	unsigned long long idle; // (4)
	double idle_diff;
	unsigned long long iowait; // (5)
	double iowait_diff;
	unsigned long long irq; // (6)
	unsigned long long softirq; // (7)
	unsigned long long steal; // (8)
	double steal_diff;
	unsigned long long uptime;
	unsigned long long uptime0;
} cpu_stat;

typedef struct {
	unsigned long long uptime;
	unsigned long long ctxt;
	unsigned long ctxt_diff;
	unsigned long procs_running;
	unsigned long procs_blocked;
	cpu_stat cpu;
	load_avg load_avg;
	meminfo mem;
	char *sysname;
	char *hostname;
} system_stat;

void system_stats_init(void);
system_stat get_system_stats(void);

#endif /* _SYSTEM_STATS_H_ */
