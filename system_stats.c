#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "postgres.h"

#include "system_stats.h"

#define FREE(v) do {if (v != NULL) {pfree(v); v = NULL;}} while(0)

#define PROC_OVERCOMMIT "/proc/sys/vm/overcommit_"

extern char *memory_cgroup_mount;
static char *memory_cgroup = NULL;
static int memory_cgroup_len = 0;

static char *sysname = "Linux";
static char *nodename = NULL;

static system_stat system_stats_old;

static int proc_read_int(const char *name)
{
	int ret = 0;
	FILE *f = fopen(name, "r");
	if (f != NULL) {
		if (fscanf(f, "%d", &ret) == 1);
		fclose(f);
	}
	return ret;
}

static load_avg read_load_avg(void)
{
	load_avg ret = {0, };
	FILE *f = fopen("/proc/loadavg", "r");
	if (f != NULL) {
		if (fscanf(f, "%f %f %f", &ret.run_1min, &ret.run_5min, &ret.run_15min) == 3);
		fclose(f);
	}
	return ret;
}

static overcommit read_overcommit(void)
{
	overcommit oc = {0,};
	oc.memory = proc_read_int(PROC_OVERCOMMIT "memory");
	oc.ratio = proc_read_int(PROC_OVERCOMMIT "ratio");
	return oc;
}

static unsigned long cgroup_read_ulong(const char *name)
{
	FILE *f;
	unsigned long ret = 0;
	strcpy(memory_cgroup + memory_cgroup_len, name);
	if ((f = fopen(memory_cgroup, "r")) != NULL) {
		if (fscanf(f, "%lu", &ret) == 1);
		fclose(f);
	}
	return ret;
}

static cgroup_memory read_cgroup_memory_stats(void)
{
	FILE *csfd;
	int i = 0, j = 0;
	cgroup_memory cm = {0,};
	char name[6], buf[255];
	unsigned long value;
	struct _mem_tab {
		const char *name;
		unsigned long *value;
	} mem_tab[] = {
		{"cache", &cm.cache},
		{"rss", &cm.rss},
		{NULL, NULL}
	};

	cm.available = true;
	cm.limit = cgroup_read_ulong("limit_in_bytes") / 1024;
	cm.usage = cgroup_read_ulong("usage_in_bytes") / 1024;

	strcpy(memory_cgroup + memory_cgroup_len, "stat");
	if ((csfd = fopen(memory_cgroup, "r")) == NULL)
		return cm;

	while (i < sizeof(mem_tab)/sizeof(struct _mem_tab) - 1
			&& fgets(buf, sizeof(buf), csfd)
			&& sscanf(buf, "%5s %lu", name, &value)) {
		for (j = 0; mem_tab[j].name != NULL; ++j) {
			if (strcmp(mem_tab[j].name, name) == 0) {
				++i;
				*mem_tab[j].value = value / 1024;
				break;
			}
		}
	}

	return cm;
}

static meminfo read_meminfo(void)
{
	FILE *mifd;
	int i = 0, j = 0;
	meminfo mi = {0,};
	char *dpos, buf[255];
	const char delimiter[] = ": ";
	struct _mem_tab {
		const char *name;
		unsigned long *value;
	} mem_tab[] = {
		{"MemTotal", &mi.total},
		{"MemFree", &mi.free},
		{"Buffers", &mi.buffers},
		{"Cached", &mi.cached},
		{"Dirty", &mi.dirty},
		{"CommitLimit", &mi.limit},
		{"Committed_AS", &mi.as},
		{NULL, NULL}
	};

	if (memory_cgroup != NULL)
		mi.cgroup = read_cgroup_memory_stats();

	if ((mifd = fopen("/proc/meminfo", "r")) == NULL)
		return mi;

	while (fgets(buf, sizeof(buf), mifd) && i < sizeof(mem_tab)/sizeof(struct _mem_tab) - 1) {
		if ((dpos = strstr(buf, delimiter)) == NULL)
			continue;

		for (j = 0; mem_tab[j].name != NULL; ++j)
			if (strncmp(mem_tab[j].name, buf, dpos - buf) == 0) {
				char unit[8];
				++i;
				if (sscanf(dpos + sizeof(delimiter) - 1, "%lu %s", mem_tab[j].value, unit) == 2) {
					if (unit[1] == 'B') {
						if (unit[0] == 'g')
							*mem_tab[j].value *= 1048576;
						else if (unit[0] == 'm')
							*mem_tab[j].value *= 1024;
					}
					mi.available = true;
				}
				break;
			}
	}

	fclose(mifd);
	mi.overcommit = read_overcommit();
	return mi;
}

static system_stat read_proc_stat(void)
{
	system_stat st = {0,};
	char buf[255], param[32];
	unsigned long long value;

	FILE *stfd = fopen("/proc/stat", "r");

	if (stfd == NULL)
		return st;

	while (fgets(buf, sizeof(buf), stfd))
		if (sscanf(buf, "%31s %llu", param, &value) == 2) {
			if (strcmp(param, "cpu") == 0) {
				cpu_stat *cs = &st.cpu;
				cs->fields = sscanf(buf, "%*s %llu %*u %llu %llu %llu %llu %llu %llu %llu",
					&cs->utime, &cs->stime, &cs->idle, &cs->iowait, &cs->irq,
					&cs->softirq, &cs->steal, &cs->guest);

				cs->total = cs->utime + cs->stime + cs->idle + cs->iowait
							+ cs->irq + cs->softirq + cs->steal + cs->guest;
			} else if (strcmp(param, "ctxt") == 0)
				st.ctxt = value;
			else if (strcmp(param, "procs_running") == 0)
				st.procs_running = value;
			else if (strcmp(param, "procs_blocked") == 0)
				st.procs_blocked = value;
		}
	fclose(stfd);
	return st;
}

static char *get_hostname()
{
	struct utsname un;
	if (uname(&un) == 0 && (nodename == NULL || strcmp(nodename, un.nodename))) {
		FREE(nodename);
		nodename = pstrdup(un.nodename);
	}
	return nodename;
}

static void diff_system_stats(system_stat *new_stats)
{
	double time_diff;
	if (system_stats_old.time.tv_sec == 0) return;
	time_diff = new_stats->time.tv_sec + new_stats->time.tv_usec/1000000.0 -
		system_stats_old.time.tv_sec - system_stats_old.time.tv_usec/1000000.0;

	new_stats->ctxt_diff = (new_stats->ctxt - system_stats_old.ctxt)/time_diff;

	time_diff = (new_stats->cpu.total - system_stats_old.cpu.total)/100.0;
	new_stats->cpu.utime_diff = (new_stats->cpu.utime - system_stats_old.cpu.utime)/time_diff;
	new_stats->cpu.stime_diff = (new_stats->cpu.stime - system_stats_old.cpu.stime)/time_diff;
	new_stats->cpu.idle_diff = (new_stats->cpu.idle - system_stats_old.cpu.idle)/time_diff;
	new_stats->cpu.iowait_diff = (new_stats->cpu.iowait - system_stats_old.cpu.iowait)/time_diff;
	new_stats->cpu.irq_diff = (new_stats->cpu.irq - system_stats_old.cpu.irq)/time_diff;
	new_stats->cpu.softirq_diff = (new_stats->cpu.softirq - system_stats_old.cpu.softirq)/time_diff;
	new_stats->cpu.steal_diff = (new_stats->cpu.steal - system_stats_old.cpu.steal)/time_diff;
	new_stats->cpu.guest_diff = (new_stats->cpu.guest - system_stats_old.cpu.guest)/time_diff;
}

system_stat get_system_stats(void)
{
	struct timezone tz;
	system_stat system_stats = read_proc_stat();
	system_stats.cpu.cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
	system_stats.uptime = proc_read_int("/proc/uptime");
	system_stats.load_avg = read_load_avg();
	system_stats.mem = read_meminfo();
	system_stats.sysname = sysname;
	system_stats.hostname = get_hostname();
	gettimeofday(&system_stats.time, &tz);
	diff_system_stats(&system_stats);
	return system_stats_old = system_stats;
}

void system_stats_init(void)
{
	struct utsname un;
	if (uname(&un) == 0)
		sysname = pstrdup(un.release);

	if (memory_cgroup_mount != NULL) {
		const char prefix[] = "/memory.";
		memory_cgroup_len = strlen(memory_cgroup_mount);
		memory_cgroup = repalloc(memory_cgroup_mount, memory_cgroup_len + 23);
		strcpy(memory_cgroup + memory_cgroup_len, prefix);
		memory_cgroup_len += sizeof(prefix) - 1;
	}
}
