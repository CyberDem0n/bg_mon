#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>

#define FATAL 1
#define LOG 1
#define elog(level, format, ...) fprintf(stderr, format "\n", __VA_ARGS__)

static unsigned long long mem_page_size;

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
} proc_stat;

typedef struct proc_io {
	unsigned long long read_bytes;
	unsigned long long write_bytes;
} proc_io;

static void traverse_procfs(pid_t ppid);
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
	const char read_bytes[] = "read_bytes: ";
	const char write_bytes[] = "write_bytes: ";
	proc_io pi = {0, 0};
	unsigned long long *value;
	int count = 0;
	char buf[255];

	FILE *iofd = open_proc_file(pid, "io");

	if (iofd == NULL)
		return pi;

	while (fgets(buf, sizeof(buf), iofd)) {
		if (!strncmp(buf, read_bytes, sizeof(read_bytes) - 1))
			value = &pi.read_bytes;
		else if (!strncmp(buf, write_bytes, sizeof(write_bytes) - 1))
			value = &pi.write_bytes;
		else continue;

		if (sscanf(buf, "%*[^:]: %llu", value) != 1) {
			pi.read_bytes = pi.write_bytes = 0;
			break;
		}

		if (++count == 2) break;
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

	ps.fields = fscanf(statfd, "%d (%*[^)]) %c %d %*d %*d %*d %*d \
%*u %*u %*u %*u %*u %lu %lu %*d %*d %ld %*d %*d %*d %llu %lu %ld %*u %*u \
%*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*d %*d %*u %*u %llu %lu",
		&ps.pid, &ps.state, &ps.ppid, &ps.utime, &ps.stime,
		&ps.priority, &ps.start_time, &ps.vsize, &ps.rss,
		&ps.delayacct_blkio_ticks, &ps.guest_time);

	if (ps.fields < 9) {
		elog(LOG, "Can't parse content of /proc/%d/stat", pid);
		ps.fields = ps.ppid = 0;
	}

	fclose(statfd);
	return ps;
}

void traverse_procfs(pid_t ppid)
{
	DIR *proc;
	struct dirent *dp;
	pid_t pid;
	proc_stat ps;
	proc_io pi;

	if ((proc = opendir("/proc")) == NULL) {
		perror("couldn't open '.'");
		return;
	}

	while ((dp = readdir(proc)) != NULL) {
		if (sscanf(dp->d_name, "%d", &pid) == 1) {
			ps = read_proc_stat(pid);
			if (ps.fields && ps.ppid == ppid) {
				pi = read_proc_io(pid);
				printf("pid=%d uss=%llu read_bytes=%llu write_bytes=%llu\n",
					 pid, get_memory_usage(pid), pi.read_bytes, pi.write_bytes);
			}
		}
		errno = 0;
	}

	if (errno != 0)
		perror("error reading directory");

	closedir(proc);
}

int main(int argc, char **argv)
{
	mem_page_size = getpagesize();
	traverse_procfs(4616);
	return 0;
}
