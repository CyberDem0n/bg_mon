#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>

#define FREE(v) do {if (v != NULL) {free(v); v = NULL;}} while(0)
#define FATAL 1
#define LOG 1
#define elog(level, format, ...) fprintf(stderr, format "\n", __VA_ARGS__)
#define false 0
#define true 1
typedef char bool;

static unsigned long long mem_page_size;

typedef struct proc_io {
	bool available;
	unsigned long long rchar;
	unsigned long long wchar;
	unsigned long long syscr;
	unsigned long long syscw;
	unsigned long long read_bytes;
	unsigned long long write_bytes;
} proc_io;

typedef struct proc_stat {
	int fields;
	pid_t pid; // (1)
	char state; // (3)
	pid_t ppid; // (4)
	unsigned long utime; // (14)
	unsigned long stime; // (15)
	long priority; // (18)
	unsigned long long start_time; // (22)
	unsigned long vsize; // (23)
	long rss; // (24)
	unsigned long long delayacct_blkio_ticks; // (42)
	unsigned long guest_time; // (43)
	unsigned long long uss;
	proc_io io;
} proc_stat;

typedef struct proc_stats {
	proc_stat *values;
	size_t size;
	size_t pos;
} proc_stats;

static void traverse_procfs(pid_t ppid, proc_stats *list);
static proc_stat read_proc_stat(pid_t pid);
static proc_io read_proc_io(pid_t pid);
static unsigned long long get_memory_usage(pid_t pid);

static FILE *open_proc_file(pid_t pid, const char *type)
{
	char proc_file[32];
	sprintf(proc_file, "/proc/%d/%s", pid, type);
	return fopen(proc_file, "r");
}

static unsigned long long get_memory_usage(pid_t pid)
{
	int resident, share;
	FILE *fd = open_proc_file(pid, "statm");
	if (fd == NULL) return 0;
	if (fscanf(fd, "%*d %d %d", &resident, &share) != 2)
		resident = share = 0;
	fclose(fd);
	return (unsigned long long)(resident - share) * mem_page_size;
}

static proc_io read_proc_io(pid_t pid)
{
	int j = 0, i = 0;
	proc_io pi = {0,};
	char *dpos, buf[255];
	const char delimiter[] = ": ";
	struct _io_tab {
		const char *name;
		unsigned long long *value;
	} io_tab[] = {
		{"rchar", &pi.rchar},
		{"wchar", &pi.wchar},
		{"syscr", &pi.syscr},
		{"syscw", &pi.syscw},
		{"read_bytes", &pi.read_bytes},
		{"write_bytes", &pi.write_bytes},
		{NULL, NULL}
	};

	FILE *iofd = open_proc_file(pid, "io");

	if (iofd == NULL)
		return pi;

	while (fgets(buf, sizeof(buf), iofd) && i++ < 8) {
		if ((dpos = strstr(buf, delimiter)) == NULL)
			continue;

		while (io_tab[j].name != NULL) {
			if (strncmp(io_tab[j].name, buf, dpos - buf) == 0) {
				if (sscanf(buf + sizeof(delimiter) - 1, "%llu", io_tab[j].value) == 1)
					pi.available = true;
				break;
			}
			j++;
		}
	}

	fclose(iofd);
	return pi;
}

static proc_stat read_proc_stat(pid_t pid)
{
	proc_stat ps = {0, 0};
	FILE *statfd = open_proc_file(pid, "stat");

	if (statfd == NULL)
		return ps;

	ps.fields = fscanf(statfd, "%d (%*[^)]) %c %d %*d %*d %*d %*d %*u %*u %*u \
%*u %*u %lu %lu %*d %*d %ld %*d %*d %*d %llu %lu %ld %*u %*u %*u %*u %*u %*u \
%*u %*u %*u %*u %*u %*u %*u %*d %*d %*u %*u %llu %lu", &ps.pid, &ps.state,
		&ps.ppid, &ps.utime, &ps.stime, &ps.priority, &ps.start_time, &ps.vsize,
		&ps.rss, &ps.delayacct_blkio_ticks, &ps.guest_time);

	if (ps.fields < 9) {
		elog(LOG, "Can't parse content of /proc/%d/stat", pid);
		ps.fields = ps.ppid = 0;
	}

	fclose(statfd);
	return ps;
}

static bool proc_stats_add(proc_stats *list, proc_stat ps)
{
	if (list->values == NULL)
		list->pos = list->size = 0;

	if (list->pos >= list->size) {
		int new_size = list->size > 0 ? list->size * 2 : 7;
		proc_stat *nvalues = (proc_stat *)realloc(list->values, sizeof(proc_stat)*new_size);
		if (nvalues == NULL) {
			elog(LOG, "Can't allocate %lu memory for proc_stats",
						sizeof(proc_stat)*new_size);
			return false;
		}
		list->size = new_size;
		list->values = nvalues;
	}
	list->values[list->pos++] = ps;
	return true;
}

static int proc_cmp(const void *el1, const void *el2)
{
	return ((const proc_stat *) el1)->pid - ((const proc_stat *) el2)->pid;
}

static void traverse_procfs(pid_t ppid, proc_stats *list)
{
	DIR *proc;
	struct dirent *dp;
	pid_t pid;
	proc_stat ps;

	if ((proc = opendir("/proc")) == NULL) {
		perror("couldn't open '/proc'");
		return;
	}

	while ((dp = readdir(proc)) != NULL) {
		if (sscanf(dp->d_name, "%d", &pid) == 1) {
			ps = read_proc_stat(pid);
			if (ps.fields && ps.ppid == ppid) {
				ps.uss = get_memory_usage(pid);
				ps.io = read_proc_io(pid);
				proc_stats_add(list, ps);
			}
		}
		errno = 0;
	}

	if (errno != 0)
		perror("error reading directory");

	closedir(proc);

	if (list->values != NULL)
		qsort(list->values, list->pos, sizeof(proc_stat), proc_cmp);

/*	for (size_t i = 0; i < list->pos; ++i) {
		ps = list->values[i];
		printf("pid=%d uss=%llu read_bytes=%llu write_bytes=%llu\n",
				ps.pid, ps.uss, ps.io.read_bytes, ps.io.write_bytes);
	}*/
}

static void calculate_diff(proc_stat *prev, proc_stat *cur)
{
	printf("pid=%d uss=%lld read=%llu write=%llu\n", prev->pid,
			cur->uss - prev->uss,
			cur->io.read_bytes - prev->io.read_bytes,
			cur->io.write_bytes - prev->io.write_bytes);
}

void run(pid_t ppid) {
	int i = 0;
	proc_stats prev = {NULL, 0, 0};
	proc_stats cur = {NULL, 0, 0};
	proc_stats tmp;

	traverse_procfs(ppid, &prev);

	for (i = 0; i < 10; i++) {
		size_t prev_pos = 0, cur_pos = 0;
		traverse_procfs(ppid, &cur);
		while (prev_pos < prev.pos && cur_pos < cur.pos) {
			if (prev.values[prev_pos].pid == cur.values[cur_pos].pid)
				calculate_diff(prev.values+prev_pos++, cur.values+cur_pos++);
			else if (prev.values[prev_pos].pid > cur.values[cur_pos].pid)
				++cur_pos;
			else ++prev_pos; // pre.pid < cur.pid
		}
		sleep(1);
		tmp = prev;
		prev = cur;
		cur = tmp;
		cur.pos = 0;
	}

	FREE(prev.values);
	FREE(cur.values);
}

int main(int argc, char **argv)
{
	mem_page_size = getpagesize();
	run(1);
	return 0;
}
