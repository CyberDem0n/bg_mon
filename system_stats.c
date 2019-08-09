#include <sys/utsname.h>
#include <unistd.h>

#include "system_stats.h"

#define PROC_OVERCOMMIT "/proc/sys/vm/overcommit_"

extern char *cpu_cgroup_mount;
static char *cpu_cgroup = NULL;
static int cpu_cgroup_len = 0;

extern char *cpuacct_cgroup_mount;
static char *cpuacct_cgroup = NULL;
static int cpuacct_cgroup_len = 0;

extern char *memory_cgroup_mount;
static char *memory_cgroup = NULL;
static int memory_cgroup_len = 0;

static char *sysname = "Linux";
static char *nodename = NULL;

system_stat system_stats_old;

static unsigned long long proc_read_uptime()
{
	unsigned long long ret = 0;
	unsigned long sec, cent;
	FILE *f = fopen("/proc/uptime", "r");
	if (f != NULL) {
		if (fscanf(f, "%lu.%lu", &sec, &cent) == 2)
			ret = (unsigned long long) sec * SC_CLK_TCK +
				(unsigned long long) cent * SC_CLK_TCK / 100;
		fclose(f);
	}
	return ret;
}

static int proc_read_int(const char *name)
{
	int ret = 0;
	FILE *f = fopen(name, "r");
	if (f != NULL) {
		if (fscanf(f, "%d", &ret) != 1) {}
		fclose(f);
	}
	return ret;
}

static load_avg read_load_avg(void)
{
	load_avg ret = {0, };
	FILE *f = fopen("/proc/loadavg", "r");
	if (f != NULL) {
		if (fscanf(f, "%f %f %f", &ret.run_1min, &ret.run_5min, &ret.run_15min) != 3) {}
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
		if (fscanf(f, "%lu", &ret) != 1) {}
		fclose(f);
	}
	return ret;
}

static unsigned long long read_ullong(const char *filename)
{
	FILE *f;
	unsigned long long ret = 0;
	if ((f = fopen(filename, "r")) != NULL) {
		if (fscanf(f, "%llu", &ret) != 1) {}
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

	while (i < lengthof(mem_tab) - 1
			&& fgets(buf, sizeof(buf), csfd)
			&& sscanf(buf, "%5s %lu", name, &value) == 2) {
		for (j = 0; mem_tab[j].name != NULL; ++j) {
			if (strcmp(mem_tab[j].name, name) == 0) {
				++i;
				*mem_tab[j].value = value / 1024;
				break;
			}
		}
	}
	fclose(csfd);

	return cm;
}

static meminfo read_meminfo(void)
{
	FILE *mifd;
	int i = 0, j = 0;
	meminfo mi = {0,};
	char *dpos, buf[255];
	const char delimiter[] = ": ";
	unsigned long slab_reclaimable = 0;
	struct _mem_tab {
		const char *name;
		unsigned long *value;
	} mem_tab[] = {
		{"MemTotal", &mi.total},
		{"MemFree", &mi.free},
		{"Buffers", &mi.buffers},
		{"Cached", &mi.cached},
		{"Dirty", &mi.dirty},
		{"SReclaimable", &slab_reclaimable},
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
	mi.cached += slab_reclaimable;
	mi.overcommit = read_overcommit();
	return mi;
}

static cgroup_cpu read_cgroup_cpu_stats(void)
{
	FILE *csfd;
	cgroup_cpu cc = {0,};

	cc.available = true;

	if (cpu_cgroup != NULL) {
		strcpy(cpu_cgroup + cpu_cgroup_len, "cfs_quota_us");
		if ((csfd = fopen(cpu_cgroup, "r")) != NULL) {
			if (fscanf(csfd, "%lld", &cc.quota) != 2) {}
			fclose(csfd);
		}

		strcpy(cpu_cgroup + cpu_cgroup_len, "shares");
		cc.shares = read_ullong(cpu_cgroup);
	}

	if (cpuacct_cgroup != NULL) {
		int i = 0, j = 0;
		char name[7], buf[255];
		unsigned long long value;
		struct _cpu_tab {
			const char *name;
			unsigned long long *value;
		} cpu_tab[] = {
			{"user", &cc.user},
			{"system", &cc.system},
			{NULL, NULL}
		};

		strcpy(cpuacct_cgroup + cpuacct_cgroup_len, "usage");
		/* nanosecondsInSecond = 1000000000.0; */
		cc.total = read_ullong(cpuacct_cgroup) / 1000000000.0 * SC_CLK_TCK;

		strcpy(cpuacct_cgroup + cpuacct_cgroup_len, "usage_percpu");
		if ((csfd = fopen(cpuacct_cgroup, "r")) != NULL) {
			while (fscanf(csfd, "%*u ") == 0)
				++cc.online_cpus;
			fclose(csfd);
		}

		strcpy(cpuacct_cgroup + cpuacct_cgroup_len, "stat");
		if ((csfd = fopen(cpuacct_cgroup, "r")) == NULL)
			return cc;

		while (i < lengthof(cpu_tab) -1
				&& fgets(buf, sizeof(buf), csfd)
				&& sscanf(buf, "%6s %llu", name, &value) == 2)
			for (j = 0; cpu_tab[j].name != NULL; ++j)
				if (strcmp(cpu_tab[j].name, name) == 0) {
					++i;
					*cpu_tab[j].value = value;
					break;
				}

		fclose(csfd);
	}

	return cc;
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
				cs->fields = sscanf(buf, "%*s %llu %llu %llu %llu %llu %llu %llu %llu",
					&cs->utime, &cs->ntime, &cs->stime, &cs->idle, &cs->iowait, &cs->irq, &cs->softirq, &cs->steal);

				cs->uptime = cs->utime + cs->ntime + cs->stime + cs->idle
							+ cs->iowait + cs->irq + cs->softirq + cs->steal;
			} else if (!st.cpu.uptime0 && strncmp(param, "cpu", 3) == 0) {
				unsigned long long utime, ntime, stime, idle, iowait, irq, softirq, steal;
				sscanf(buf, "%*s %llu %llu %llu %llu %llu %llu %llu %llu",
					&utime, &ntime, &stime, &idle, &iowait, &irq, &softirq, &steal);
				st.cpu.uptime0 = utime + ntime + stime + idle + iowait + irq + softirq + steal;
			} else if (strcmp(param, "ctxt") == 0)
				st.ctxt = value;
			else if (strcmp(param, "procs_running") == 0)
				st.procs_running = value;
			else if (strcmp(param, "procs_blocked") == 0)
				st.procs_blocked = value;
		}
	fclose(stfd);

	if (cpu_cgroup != NULL || cpuacct_cgroup != NULL)
		st.cpu.cgroup = read_cgroup_cpu_stats();
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
	unsigned long long itv;
	if (system_stats_old.cpu.uptime == 0) return;
	itv = new_stats->cpu.uptime - system_stats_old.cpu.uptime;
	
	new_stats->cpu.utime_diff = SP_VALUE(system_stats_old.cpu.utime, new_stats->cpu.utime, itv);
	new_stats->cpu.ntime_diff = SP_VALUE(system_stats_old.cpu.ntime, new_stats->cpu.ntime, itv);

	/* Time spent in system mode also includes time spent servicing hard and soft interrupts. */
	new_stats->cpu.stime_diff = SP_VALUE(system_stats_old.cpu.stime + system_stats_old.cpu.irq
		+ system_stats_old.cpu.softirq, new_stats->cpu.stime + new_stats->cpu.irq + new_stats->cpu.softirq, itv);

	new_stats->cpu.iowait_diff = SP_VALUE(system_stats_old.cpu.iowait, new_stats->cpu.iowait, itv);
	new_stats->cpu.steal_diff = SP_VALUE(system_stats_old.cpu.steal, new_stats->cpu.steal, itv);

	new_stats->cpu.idle_diff = new_stats->cpu.idle < system_stats_old.cpu.idle ? 0.0:
		SP_VALUE(system_stats_old.cpu.idle, new_stats->cpu.idle, itv);

	if (new_stats->cpu.cgroup.available) {
		double total = new_stats->cpu.cgroup.online_cpus
			* SP_VALUE_100(system_stats_old.cpu.cgroup.total, new_stats->cpu.cgroup.total, itv);
		long long system_diff = new_stats->cpu.cgroup.system - system_stats_old.cpu.cgroup.system;
		long long user_diff = new_stats->cpu.cgroup.user - system_stats_old.cpu.cgroup.user;
		long long sum_diff = system_diff + user_diff;

		if (total > 0 && sum_diff > 0) {
			new_stats->cpu.cgroup.system_diff = system_diff > 0 ? total * system_diff / sum_diff : 0;
			new_stats->cpu.cgroup.user_diff = user_diff > 0 ? total * user_diff / sum_diff : 0;
		}
	}

	if (new_stats->cpu.cpu_count > 1) itv = new_stats->uptime - system_stats_old.uptime;
	new_stats->ctxt_diff = S_VALUE(system_stats_old.ctxt, new_stats->ctxt, itv);
}

system_stat get_system_stats(void)
{
	system_stat system_stats = read_proc_stat();
	system_stats.cpu.cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
	system_stats.uptime = proc_read_uptime();
	if (system_stats.uptime == 0)
		system_stats.uptime = system_stats.cpu.uptime0;
	system_stats.load_avg = read_load_avg();
	system_stats.mem = read_meminfo();
	system_stats.sysname = sysname;
	system_stats.hostname = get_hostname();
	diff_system_stats(&system_stats);
	return system_stats_old = system_stats;
}

void system_stats_init(void)
{
	struct utsname un;
	SC_CLK_TCK = sysconf(_SC_CLK_TCK);

	if (uname(&un) == 0)
		sysname = pstrdup(un.release);

	if (cpu_cgroup_mount != NULL) {
		const char prefix[] = "/cpu.";
		cpu_cgroup_len = strlen(cpu_cgroup_mount);
		cpu_cgroup = repalloc(cpu_cgroup_mount, cpu_cgroup_len + 18); /* strlen("/cpu.cfs_quota_us") + 1 */
		strcpy(cpu_cgroup + cpu_cgroup_len, prefix);
		cpu_cgroup_len += sizeof(prefix) - 1;
	}

	if (cpuacct_cgroup_mount != NULL) {
		const char prefix[] = "/cpuacct.";
		cpuacct_cgroup_len = strlen(cpuacct_cgroup_mount);
		cpuacct_cgroup = repalloc(cpuacct_cgroup_mount, cpuacct_cgroup_len + 22); /* strlen("/cpuacct.usage_percpu") + 1 */
		strcpy(cpuacct_cgroup + cpuacct_cgroup_len, prefix);
		cpuacct_cgroup_len += sizeof(prefix) - 1;
	}

	if (memory_cgroup_mount != NULL) {
		const char prefix[] = "/memory.";
		memory_cgroup_len = strlen(memory_cgroup_mount);
		memory_cgroup = repalloc(memory_cgroup_mount, memory_cgroup_len + 23); /* strlen("/memory.usage_in_bytes") + 1 */
		strcpy(memory_cgroup + memory_cgroup_len, prefix);
		memory_cgroup_len += sizeof(prefix) - 1;
	}
}
