#ifndef _SYSTEM_STATS_H_
#define _SYSTEM_STATS_H_

#include "postgres.h"

extern double SC_CLK_TCK;

#define MINIMUM(a,b)		((a) < (b) ? (a) : (b))
#define MAXIMUM(a,b)		((a) > (b) ? (a) : (b))
#define FREE(v)				do {if (v != NULL) {pfree(v); v = NULL;}} while(0)
#define S_VALUE(m,n,p)		(((double) ((n) - (m))) / (p) * SC_CLK_TCK)
#define SP_VALUE(m,n,p)		(n < m ? 0 : ((double) ((n) - (m))) / (p) * 100)
#define SP_VALUE_100(m,n,p)	MINIMUM((((double) ((n) - (m))) / (p) * 100), 100.0)

typedef struct {
	float run_1min;
	float run_5min;
	float run_15min;
} load_avg;

#define PTYPE_SIZE 4

/*
 * The PSI feature identifies and quantifies the disruptions caused by resource
 * contention and the time impact it has on complex workloads or even entire
 * systems. This information is exposed in time shares, the ratios (in %) are
 * tracked as recent trends over ten, sixty, three hundred second windows.
 * Total time is exposed as well in us (1.0E-6 seconds).
 *
 * Monitored resources are cpu, memory or io. Pressure types:
 *
 * - "some" indicates the share of time in which at least some tasks are
 *   stalled on a given resource, but the CPU is still doing productive work.
 *
 * - "full" indicates the share of time in which all non-idle tasks are stalled
 *   on a given resource simultaneously. In this state actual CPU cycles are
 *   going to waste, and a workload that spends extended time in this state is
 *   considered to be thrashing.
 *
 * Pressure for CPU contains only type "some", while Memory and IO have both
 * types.
 */
typedef enum pressure_type
{
	UNDEFINED = 0,
	SOME,
	FULL,
} pressure_type;

typedef enum pressure_res
{
	CPU = 0,
	MEMORY,
	IO,
} pressure_res;

typedef struct {
	pressure_type type;
	float avg10;
	float avg60;
	float avg300;
	uint64 total;
} pressure;

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
	unsigned long dirty;
	unsigned long oom_kill;
	unsigned long failcnt;
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
	bool available;
	unsigned int online_cpus;
	unsigned long long shares;
	long long quota;
	unsigned long long total;
	unsigned long long system;
	double system_diff;
	unsigned long long user;
	double user_diff;
} cgroup_cpu;

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
	cgroup_cpu cgroup;
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
	bool pressure;			/* tells if PSI information is available */
	pressure p_cpu[2];		/* PSI for CPU. Only of type "some" is used, but
							   for consistensy with other resources defined as
							   two elements array. */
	pressure p_memory[2];	/* PSI for Memory (pair "some", "full") */
	pressure p_io[2];		/* PSI for IO (pair "some", "full") */
	char *sysname;
	char *hostname;
} system_stat;

void system_stats_init(void);
system_stat get_system_stats(void);

#endif /* _SYSTEM_STATS_H_ */
