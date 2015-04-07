#ifndef _SYSTEM_STATS_H_
#define _SYSTEM_STATS_H_

typedef char bool;

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
	unsigned long long stime; // (3)
	double stime_diff;
	unsigned long long idle; // (4)
	double idle_diff;
	unsigned long long iowait; // (5)
	double iowait_diff;
	unsigned long long irq; // (6)
	double irq_diff;
	unsigned long long softirq; // (7)
	double softirq_diff;
	unsigned long long steal; // (8)
	double steal_diff;
	unsigned long long guest; // (9)
	double guest_diff;
	unsigned long long total;
} cpu_stat;

typedef struct {
	unsigned long long ctxt;
	unsigned long ctxt_diff;
	unsigned long procs_running;
	unsigned long procs_blocked;
	cpu_stat cpu;
	int uptime;
	load_avg load_avg;
	meminfo mem;
	struct timeval time;
	char *sysname;
	char *hostname;
} system_stat;

void system_stats_init(void);
system_stat get_system_stats(void);

#endif /* _SYSTEM_STATS_H_ */
