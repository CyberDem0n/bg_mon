#include <mntent.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <linux/limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/sysmacros.h>

#include "postgres.h"

#include "tcop/utility.h"
#include "access/xlog_internal.h"
#include "postmaster/syslogger.h"

#include "system_stats.h"
#include "disk_stats.h"

const int DATA = 0;
const int WAL = 1;
const int LOGS = 2;

extern system_stat system_stats_old;

typedef struct {
	char *me_devname;
	char *me_mountdir;
	dev_t me_dev;
	bool me_dummy;
	bool me_remote;
} mount_entry;

extern char *DataDir;
static char *wal_directory;
static char *log_directory;
static char *data_dev;
static char *wal_dev;
static char *log_dev;

static disk_stats disk_stats_old = {0,};
static disk_stats disk_stats_next = {0,};
static pthread_mutex_t du_lock;
static pthread_cond_t du_cond;
static bool run_du;
static unsigned long long data_du, wal_du, log_du;
static unsigned int du_counter;

char *cpu_cgroup_mount = NULL;
char *cpuacct_cgroup_mount = NULL;
char *memory_cgroup_mount = NULL;
char *cgroup2_mount = NULL;

/******************************************************
 * implementation of du -s and du -sx functionality for
 * calculating size of data_directory
 * works recursively, returns total size in kilobytes.
 * optionally can calculate size of pg_wal
 ******************************************************/
static unsigned long long du(int dirfd, const char *path, dev_t dev, unsigned long long *wal)
{
	struct stat st;
	unsigned long long ret = 0;
	struct dirent *e = NULL;
	DIR *dir;

	if (fstatat(dirfd, path, &st, dev == 0 ? 0 : AT_SYMLINK_NOFOLLOW))
		return ret;

	if (dev == 0) dev = st.st_dev;
	else if (st.st_dev != dev) return ret;

	ret += st.st_blocks/2;

	if ((st.st_mode & S_IFMT) != S_IFDIR
			|| (dirfd = openat(dirfd, path, O_RDONLY|O_NOCTTY|O_DIRECTORY)) == -1)
		return ret;

	if ((dir = fdopendir(dirfd)) == NULL) {
		close(dirfd);
		return ret;
	}

	while (errno = 0, NULL != (e = readdir(dir)))
		// skip "." and "..", don't go into lost+found
		if ((e->d_name[0] != '.'
				|| (e->d_name[1] && (e->d_name[1] != '.' || e->d_name[2])))
				&& strcmp(e->d_name, "lost+found") != 0) {

			if (wal && !strncmp(e->d_name, XLOGDIR, sizeof(XLOGDIR))) {
				*wal = du(dirfd, e->d_name, 0, NULL);
				if (fstatat(dirfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
					if ((st.st_mode & S_IFMT) == S_IFDIR)
						ret += *wal;
					else ret += st.st_blocks/2;
				}
			} else ret += du(dirfd, e->d_name, dev, NULL);
		}

	closedir(dir); // will close dirfd as well
	return ret;
}

static void *du_thread(void *arg)
{
	unsigned long long tmp_data_du, tmp_wal_du, tmp_log_du;
	while (true) {
		tmp_data_du = du(AT_FDCWD, DataDir, 0, &tmp_wal_du);
		tmp_log_du = du(AT_FDCWD, log_directory, 0, NULL);

		// log_directory is a subdirectory of DataDir
		if (path_is_prefix_of_path(DataDir, log_directory))
			tmp_data_du -= tmp_log_du;

		pthread_mutex_lock(&du_lock);
		data_du = tmp_data_du;
		wal_du = tmp_wal_du;
		log_du = tmp_log_du;
		while (!run_du)
			pthread_cond_wait(&du_cond, &du_lock);
		run_du = false;
		pthread_mutex_unlock(&du_lock);
	}

	return (void *)0;
}

static bool is_dummy(struct mntent *me)
{
	return (strcmp(me->mnt_type, "autofs") == 0
		|| strcmp(me->mnt_type, "rootfs") == 0
		|| strcmp(me->mnt_type, "proc") == 0
		|| strcmp(me->mnt_type, "subfs") == 0
		|| strcmp(me->mnt_type, "debugfs") == 0
		|| strcmp(me->mnt_type, "devpts") == 0
		|| strcmp(me->mnt_type, "fusectl") == 0
		|| strcmp(me->mnt_type, "mqueue") == 0
		|| strcmp(me->mnt_type, "rpc_pipefs") == 0
		|| strcmp(me->mnt_type, "sysfs") == 0
		|| strcmp(me->mnt_type, "devfs") == 0
		|| strcmp(me->mnt_type, "cgroup") == 0
		|| strcmp(me->mnt_type, "lofs") == 0
		|| strcmp(me->mnt_type, "none") == 0)
		&& hasmntopt(me, "bind") == NULL;
}

static bool is_remote(struct mntent *me)
{
	return (strchr(me->mnt_fsname, ':') != NULL
		|| (me->mnt_fsname[0] == '/'
			&& me->mnt_fsname[1] == '/'
			&& (strcmp(me->mnt_type, "smbfs") == 0
				|| strcmp(me->mnt_type, "cifs") == 0)));
}

static List *read_mounts()
{
	List *mounts = NIL;
	struct mntent *me;
	FILE *f;

	/* try "/proc/mounts" first and if it failed go with defaults */
	if ((f = setmntent("/proc/mounts", "r")) != NULL || (f = setmntent(MOUNTED, "r")) != NULL) {
		while ((me = getmntent(f)) != NULL) {
			mount_entry *m = palloc(sizeof(mount_entry));
			m->me_devname = pstrdup(me->mnt_fsname);
			m->me_mountdir = pstrdup(me->mnt_dir);
			m->me_dev = -1;
			m->me_dummy = is_dummy(me);
			m->me_remote = is_remote(me);
			mounts = lappend(mounts, m);
			if (cpu_cgroup_mount == NULL
					&& strcmp(me->mnt_type, "cgroup") == 0
					&& hasmntopt(me, "cpu"))
				cpu_cgroup_mount = pstrdup(me->mnt_dir);
			if (cpuacct_cgroup_mount == NULL
					&& strcmp(me->mnt_type, "cgroup") == 0
					&& hasmntopt(me, "cpuacct"))
				cpuacct_cgroup_mount = pstrdup(me->mnt_dir);
			if (memory_cgroup_mount == NULL
					&& strcmp(me->mnt_type, "cgroup") == 0
					&& hasmntopt(me, "memory"))
				memory_cgroup_mount = pstrdup(me->mnt_dir);
			if (cgroup2_mount == NULL && strcmp(me->mnt_type, "cgroup2") == 0)
				cgroup2_mount = pstrdup(me->mnt_dir);
		}
		fclose(f);
	}
	return mounts;
}

static void free_mounts(List *mounts)
{
	ListCell *lc;
	foreach (lc, mounts) {
		mount_entry *m = lfirst(lc);
		FREE(m->me_devname);
		FREE(m->me_mountdir);
	}
	list_free_deep(mounts);
}

static char *resolve_dm_name(char *mapper_name)
{
	static char dm_name[NAME_MAX];
	struct dirent *e = NULL;
	DIR *dir = opendir("/sys/block");

	if (dir == NULL) return mapper_name;

	while (errno = 0, NULL != (e = readdir(dir)))
		if (e->d_name[0] != '.' || (e->d_name[1]
				&& (e->d_name[1] != '.' || e->d_name[2]))) {
			int fd, dfd = dirfd(dir);
			size_t r = 0;

			if (dfd == -1 || (dfd = openat(dfd, e->d_name,
					O_RDONLY|O_NOCTTY|O_DIRECTORY)) == -1)
				continue;

			if ((fd = openat(dfd, "dm/name", O_RDONLY|O_NOCTTY)) != -1) {
				r = read(fd, dm_name, sizeof(dm_name) - 1);
				close(fd);
			}
			close(dfd);

			while (r > 0 && (dm_name[r - 1] == '\r' || dm_name[r - 1] == '\n'))
				dm_name[--r] = '\0';

			if (r > 0 && strncmp(mapper_name, dm_name, r) == 0) {
				strcpy(dm_name, e->d_name);
				closedir(dir);
				return dm_name;
			}
		}

	closedir(dir);
	return mapper_name;
}

static char *resolve_device_name(const char *path)
{
	static char ret[NAME_MAX];
	struct stat info;

	strcpy(ret, path + 5);  /* fallback */

	if (stat(path, &info) == 0 && S_ISBLK(info.st_mode)) {
		FILE *io = fopen("/proc/diskstats", "r");

		if (io != NULL) {
			int min, maj;
			char buf[256], device_name[128];

			while (fgets(buf, sizeof(buf), io))
				if (sscanf(buf, "%u %u %s", &maj, &min, device_name) == 3
						&& maj == major(info.st_rdev) && min == minor(info.st_rdev)) {
					strcpy(ret, device_name);
					break;
				}
			fclose(io);
		}
	}
	return ret;
}

static char *get_device(List *mounts, const char *path)
{
	ListCell *lc;
	char buf[PATH_MAX];
	struct stat disk_stats, statp;
	char *resolved = realpath(path, buf);
	mount_entry const *best_match = NULL;

	if (stat(path, &statp) != 0) return NULL;

	if (resolved && *resolved == '/') {
		size_t resolved_len = strlen (resolved);
		size_t best_match_len = 0;
		foreach (lc, mounts) {
			mount_entry *me = lfirst(lc);
			if (!best_match || best_match->me_dummy || !me->me_dummy) {
				size_t len = strlen (me->me_mountdir);
				if (best_match_len <= len && len <= resolved_len
					&& (len == 1 /* root file system */
						|| ((len == resolved_len || resolved[len] == '/')
						&& strncmp(me->me_mountdir, resolved, len) == 0))) {
					best_match = me;
					best_match_len = len;
				}
			}
		}
	}

	if (best_match
		&& (stat(best_match->me_mountdir, &disk_stats) != 0
			|| disk_stats.st_dev != statp.st_dev))
		best_match = NULL;

	if (!best_match)
		foreach (lc, mounts) { 
			mount_entry *me = lfirst(lc);
			if (me->me_dev == (dev_t) -1) { 
				if (stat(me->me_mountdir, &disk_stats) == 0)
					me->me_dev = disk_stats.st_dev;
				else {
					if (errno == EIO)
						elog(WARNING, "Can't stat on %s", me->me_mountdir);
					/* So we won't try and fail repeatedly. */
					me->me_dev = (dev_t) -2;
				}
			}

			if (statp.st_dev == me->me_dev
				&& (!best_match || best_match->me_dummy || !me->me_dummy)) {
				/* Skip bogus mtab entries. */
				if (stat(me->me_mountdir, &disk_stats) != 0
					|| disk_stats.st_dev != me->me_dev)
					me->me_dev = (dev_t) -2;
				else best_match = me;
			}
		}

	if (!best_match) return NULL;

	if (strncmp(best_match->me_devname, "/dev/mapper/", 12) == 0)
		return resolve_dm_name(best_match->me_devname + 12);

	if (strncmp(best_match->me_devname, "/dev/", 5) == 0)
		return resolve_device_name(best_match->me_devname);

	return best_match->me_devname; 
}

static void read_io_stats(device_stats *ds)
{
	int n, i = 0;
	char buf[256], device_name[128];
	unsigned long read_completed, read_merges_or_read_sectors;
	unsigned long read_sectors_or_write_completed, read_time_or_write_sectors;
	unsigned long write_completed, write_merges, write_sectors;
	unsigned int ios_in_progress, write_time, total_time, weighted_time;
	unsigned long discard_completed, discard_merges, discard_sectors, discard_time;
	unsigned long flush_completed, flush_time;
	device_stat *stats = ds->values;
	FILE *io = fopen("/proc/diskstats", "r");

	if (io == NULL) return;

	while (fgets(buf, sizeof(buf), io)) {
		i = sscanf(buf, "%*u %*u %s %lu %lu %lu %lu %lu %lu %lu %u %u %u %u %lu %lu %lu %lu %lu %lu",
					device_name, &read_completed, &read_merges_or_read_sectors,
					&read_sectors_or_write_completed, &read_time_or_write_sectors,
					&write_completed, &write_merges, &write_sectors, &write_time,
					&ios_in_progress, &total_time, &weighted_time,
					&discard_completed, &discard_merges, &discard_sectors,
					&discard_time, &flush_completed, &flush_time);
		if (i < 12 && i != 5) continue;

		for (n = 0; n < ds->size; ++n) {
			if (strcmp(stats[n].name, device_name) != 0) continue;

			stats[n].fields = i;
			if (i == 5) {
				stats[n].read_completed = read_completed;
				stats[n].read_sectors = read_merges_or_read_sectors;
				stats[n].write_completed = read_sectors_or_write_completed;
				stats[n].write_sectors = read_time_or_write_sectors;
			} else {
				stats[n].read_completed = read_completed;
				stats[n].read_merges = read_merges_or_read_sectors;
				stats[n].read_sectors = read_sectors_or_write_completed;
				stats[n].read_time = (unsigned int)read_time_or_write_sectors;
				stats[n].write_completed = write_completed;
				stats[n].write_merges = write_merges;
				stats[n].write_sectors = write_sectors;
				stats[n].write_time = write_time;
				stats[n].ios_in_progress = ios_in_progress;
				stats[n].total_time = total_time;
				stats[n].weighted_time = weighted_time;

				if (i >= 16) {
					stats[n].discard_completed = discard_completed;
					stats[n].discard_merges = discard_merges;
					stats[n].discard_sectors = discard_sectors;
					stats[n].discard_time = discard_time;

					if (i >= 18) {
						stats[n].flush_completed = flush_completed;
						stats[n].flush_time = flush_time;
					}
				}
			}
		}
	}
	fclose(io);
}

static void diff_disk_stats(device_stats *new_stats)
{
	int i, j = 0;
	device_stat o;
	unsigned long long itv;

	new_stats->uptime = system_stats_old.uptime;

	if (disk_stats_old.dstats.uptime == 0) return;

	itv = new_stats->uptime - disk_stats_old.dstats.uptime;

	for (i = 0; i < new_stats->size; ++i) {
		double cmpl_diff, tput;
		device_stat *n = new_stats->values + i;

		while (j < disk_stats_old.dstats.size && disk_stats_old.dstats.values[j].fields == 0)
			++j;

		if (j >= disk_stats_old.dstats.size)
			break;

		o = disk_stats_old.dstats.values[j++];

		/* IOPS */
		n->read_completed_diff = S_VALUE(o.read_completed, n->read_completed, itv);
		n->write_completed_diff = S_VALUE(o.write_completed, n->write_completed, itv);
		n->discard_completed_diff = S_VALUE(o.discard_completed, n->discard_completed, itv);
		n->flush_completed_diff = S_VALUE(o.flush_completed, n->flush_completed, itv);

		/* The sector size is 512 bytes. By dividing by 2.0 we get the throughput in kB/s */
		n->read_diff = S_VALUE(o.read_sectors, n->read_sectors, itv) / 2.0;
		n->write_diff = S_VALUE(o.write_sectors, n->write_sectors, itv) / 2.0;
		n->discard_diff = S_VALUE(o.discard_sectors, n->discard_sectors, itv) / 2.0;

		/* Merges/s */
		n->read_merges_diff = S_VALUE(o.read_merges, n->read_merges, itv);
		n->write_merges_diff = S_VALUE(o.write_merges, n->write_merges, itv);
		n->discard_merges_diff = S_VALUE(o.discard_merges, n->discard_merges, itv);

		cmpl_diff = (n->read_completed - o.read_completed) +
					(n->write_completed  - o.write_completed) +
					(n->discard_completed - o.discard_completed);
		tput = cmpl_diff * SC_CLK_TCK / itv / 10.0;

		n->util = MINIMUM(S_VALUE(o.total_time, n->total_time, itv) / 10.0, 100.0);
		n->average_service_time = tput ? n->util / tput : 0.0;
		n->average_request_size = cmpl_diff ? (n->read_sectors - o.read_sectors +
											   n->write_sectors - o.write_sectors +
											   n->discard_sectors - o.discard_sectors) / cmpl_diff / 2.0: 0.0;
		n->average_queue_length = S_VALUE(o.weighted_time, n->weighted_time, itv) / 1000.0;
		n->await = cmpl_diff ? (n->read_time - o.read_time +
								n->write_time - o.write_time +
								n->discard_time - o.discard_time) / cmpl_diff : 0.0;

		cmpl_diff = n->read_completed - o.read_completed;
		n->read_await = cmpl_diff ? (n->read_time - o.read_time) / cmpl_diff : 0.0;
		n->read_average_request_size = cmpl_diff ? (n->read_sectors - o.read_sectors) / cmpl_diff / 2.0 : 0.0;

		cmpl_diff = n->write_completed - o.write_completed;
		n->write_await = cmpl_diff ? (n->write_time - o.write_time) / cmpl_diff : 0.0;
		n->write_average_request_size = cmpl_diff ? (n->write_sectors - o.write_sectors) / cmpl_diff / 2.0 : 0.0;

		cmpl_diff = n->discard_completed - o.discard_completed;
		n->discard_await = cmpl_diff ? (n->discard_time - o.discard_time) / cmpl_diff : 0.0;
		n->discard_average_request_size = cmpl_diff ? (n->discard_sectors - o.discard_sectors) / cmpl_diff / 2.0 : 0.0;

		cmpl_diff = n->flush_completed - o.flush_completed;
		n->flush_await = cmpl_diff ? (n->flush_time - o.flush_time) / cmpl_diff : 0.0;
	}
}

static unsigned char find_or_add_device(device_stats *devices, const char *name);

static void resolve_device_hierarchy(device_stats *devs, int id)
{
	int len, slave_id, i = 0;
	DIR *dir;
	struct dirent *e = NULL;
	char slaves[255] = "/sys/block/";
	char path[PATH_MAX];
	char *p;

	strcpy(slaves + 11, devs->values[id].name);
	strcpy(slaves + 11 + devs->values[id].name_len, "/slaves");

	if (NULL != (dir = opendir(slaves))) {
		while (errno = 0, NULL != (e = readdir(dir)))
			if (e->d_name[0] != '.' || (e->d_name[1]
					&& (e->d_name[1] != '.' || e->d_name[2]))) {
				p = NULL;
				snprintf(path, sizeof(path), "%s/start", e->d_name);
				if (faccessat(dirfd(dir), path, R_OK, 0) == 0 &&
						(len = readlinkat(dirfd(dir), e->d_name, path, sizeof(path) - 1)) > 0) {
					path[len] = '\0';
					if ((p = strrchr(path, '/'))) {
						*p = '\0';
						if ((p = strrchr(path, '/')))
							*(p++) = '\0';
					}
				}
				slave_id = find_or_add_device(devs, p == NULL ? e->d_name : p);
				devs->values[id].slaves[i++] = slave_id;
			}
		closedir(dir);
	}
	devs->values[id].slave_size = i;
}

static unsigned char find_or_add_device(device_stats *devices, const char *name)
{
	int i, new_id = -1;

	for (i = 0; i < devices->size; ++i)
		if (0 == strcmp(devices->values[i].name, name)) {
			new_id = i;
			goto resolve_hierarchy;
		}

	if ((new_id = devices->size++) >= devices->len)
		devices->values = repalloc(devices->values, sizeof(device_stat)*(devices->len = devices->size));

	memset(devices->values + new_id, 0, sizeof(device_stat));
	devices->values[new_id].name = pstrdup(name);
	devices->values[new_id].name_len = strlen(name);

resolve_hierarchy:
	resolve_device_hierarchy(devices, new_id);
	return new_id;
}

static bool copy_device_stats(device_stats o, device_stats *n)
{
	bool ret = false;
	int i, len = 0;

	n->size = 0;

	for (i = 0; i < o.size; ++i)
		if (o.values[i].fields > 0)
			++len;
		else {
			FREE(o.values[i].name);
			ret = true;
		}

	if (len == 0) return true;
	else if (len > n->len)
		n->values = repalloc(n->values, (n->len = len)*sizeof(device_stat));

	for (i = 0; i < o.size; ++i)
		if (o.values[i].fields > 0) {
			memset(n->values + n->size, 0, sizeof(device_stat));
			if (ret) n->values[n->size].slave_size = 0;
			else {
				memcpy(n->values[n->size].slaves, o.values[i].slaves, sizeof(o.values[i].slaves));
				n->values[n->size].slave_size = o.values[i].slave_size;
			}
			n->values[n->size].name = o.values[i].name;
			n->values[n->size++].name_len = o.values[i].name_len;
		} else FREE(o.values[i].name);
	return ret;
}

disk_stats get_disk_stats(void)
{
	struct statvfs st;
	disk_stats ret = disk_stats_next;
	disk_stat *disk_stats = ret.values;

	disk_stats[DATA].type = "data";
	disk_stats[DATA].directory = DataDir;

	disk_stats[WAL].type = "wal";
	disk_stats[WAL].directory = wal_directory;

	disk_stats[LOGS].type = "log";
	disk_stats[LOGS].directory = log_directory;

	du_counter = (du_counter + 1) % 30;
	pthread_mutex_lock(&du_lock);
	disk_stats[DATA].du = data_du;
	disk_stats[WAL].du = wal_du;
	disk_stats[LOGS].du = log_du;
	if (du_counter == 0)
		run_du = true;
	pthread_mutex_unlock(&du_lock);

	if (copy_device_stats(disk_stats_old.dstats, &ret.dstats) || du_counter == 0) {
		if (data_dev)
			disk_stats[DATA].device_id = find_or_add_device(&ret.dstats, data_dev);

		if (wal_dev) {
			if (wal_dev == data_dev) disk_stats[WAL].device_id = disk_stats[DATA].device_id;
			else disk_stats[WAL].device_id = find_or_add_device(&ret.dstats, wal_dev);
		}

		if (log_dev) {
			if (log_dev == data_dev) disk_stats[LOGS].device_id = disk_stats[DATA].device_id;
			else if (log_dev == wal_dev) disk_stats[LOGS].device_id = disk_stats[WAL].device_id;
			else disk_stats[LOGS].device_id = find_or_add_device(&ret.dstats, log_dev);
		}
	}

	if (du_counter == 0)
		pthread_cond_signal(&du_cond);

	if (statvfs(DataDir, &st) == 0) {
		disk_stats[DATA].size = st.f_blocks * st.f_bsize / 1024;
		disk_stats[DATA].free = st.f_bavail * st.f_bsize / 1024;
	}

	if (data_dev == wal_dev) {
		disk_stats[WAL].size = disk_stats[DATA].size;
		disk_stats[WAL].free = disk_stats[DATA].free;
	} else if (statvfs(wal_directory, &st) == 0) {
		disk_stats[WAL].size = st.f_blocks * st.f_bsize / 1024;
		disk_stats[WAL].free = st.f_bavail * st.f_bsize / 1024;
	}
	
	if (data_dev == log_dev) {
		disk_stats[LOGS].size = disk_stats[DATA].size;
		disk_stats[LOGS].free = disk_stats[DATA].free;
	} else if (wal_dev == log_dev) {
		disk_stats[LOGS].size = disk_stats[WAL].size;
		disk_stats[LOGS].free = disk_stats[WAL].free;
	} else if (statvfs(log_directory, &st) == 0) {
		disk_stats[LOGS].size = st.f_blocks * st.f_bsize / 1024;
		disk_stats[LOGS].free = st.f_bavail * st.f_bsize / 1024;
	}

	read_io_stats(&ret.dstats);
	
	diff_disk_stats(&ret.dstats);

	disk_stats_next = disk_stats_old;

	return disk_stats_old = ret;
}

void disk_stats_init(void)
{
	pthread_t thread;
	List *mounts = read_mounts();

	char buf[PATH_MAX];
	char tmp_path[MAXPGPATH];

	// we assume wal_directory is always inside DataDir
	wal_directory = palloc(strlen(DataDir) + sizeof(XLOGDIR) + 2);
	join_path_components(wal_directory, DataDir, XLOGDIR);

	// log_directory can be either inside or outside DataDir
	if (!is_absolute_path(Log_directory))
		join_path_components(tmp_path, DataDir, Log_directory);
	else strcpy(tmp_path, Log_directory);
	log_directory = realpath(tmp_path, buf) ? pstrdup(buf) : pstrdup(tmp_path);

	data_dev = get_device(mounts, DataDir);
	if (data_dev) data_dev = pstrdup(data_dev);
	wal_dev = get_device(mounts, wal_directory);
	if (wal_dev) {
		if (data_dev && strcmp(data_dev, wal_dev) == 0)
			wal_dev = data_dev;
		else wal_dev = pstrdup(wal_dev);
	}
	log_dev = get_device(mounts, log_directory);
	if (log_dev) {
		if (data_dev && strcmp(data_dev, log_dev) == 0)
			log_dev = data_dev;
		else if (wal_dev && strcmp(wal_dev, log_dev) == 0)
			log_dev = wal_dev;
		else log_dev = pstrdup(log_dev);
	}

	free_mounts(mounts);

	disk_stats_old.dstats.values = palloc(sizeof(device_stat));
	disk_stats_old.dstats.len = 1;
	disk_stats_old.dstats.size = 0;
	disk_stats_old.dstats.uptime = 0;

	disk_stats_next.dstats.values = palloc(sizeof(device_stat));
	disk_stats_next.dstats.len = 1;
	disk_stats_next.dstats.uptime = 0;

	run_du = false;
	du_counter = 0;
	data_du = 0;
	wal_du = 0;
	log_du = 0;
	pthread_mutex_init(&du_lock, NULL);
	pthread_cond_init(&du_cond, NULL);
	pthread_create(&thread, NULL, du_thread, NULL);
}
