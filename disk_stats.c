#include <mntent.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

#include "postgres.h"

#include "tcop/utility.h"

#include "disk_stats.h"

#define FREE(v) do {if (v != NULL) {pfree(v); v = NULL;}} while(0)

typedef struct {
	char *me_devname;
	char *me_mountdir;
	char *me_type;
	dev_t me_dev;
	bool me_dummy;
	bool me_remote;
} mount_entry;

static const char pg_xlog[] = "pg_xlog";
extern char *DataDir;
static char *xlog_directory;
static char *data_dev;
static char *xlog_dev;

static disk_stat disk_stats_old = {0,};

/******************************************************
 * implementation of du -s and du -sx functionality for
 * calculating size of data_directory
 * works recursively, returns total size in kilobytes.
 * optionally can calculate size of pg_xlog
 ******************************************************/
static unsigned long long du(int dirfd, const char *path, dev_t dev, unsigned long long *xlog)
{
	struct stat st;
	unsigned long long ret = 0;
	struct dirent buf, *e;
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

	while (readdir_r(dir, &buf, &e) == 0 && e != NULL)
		// skip "." and "..", don't go into lost+found
		if ((e->d_name[0] != '.'
				|| (e->d_name[1] && (e->d_name[1] != '.' || e->d_name[2])))
				&& strcmp(e->d_name, "lost+found") != 0) {

			if (xlog && !strncmp(e->d_name, pg_xlog, sizeof(pg_xlog))) {
				*xlog = du(dirfd, e->d_name, 0, NULL);
				if (fstatat(dirfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
					if ((st.st_mode & S_IFMT) == S_IFDIR)
						ret += *xlog;
					else ret += st.st_blocks/2;
				}
			} else ret += du(dirfd, e->d_name, dev, NULL);
		}

	closedir(dir); // will close dirfd as well
	return ret;
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
	FILE *f = setmntent(MOUNTED, "r");

	while ((me = getmntent(f)) != NULL) {
		mount_entry *m = palloc(sizeof(mount_entry));
		m->me_devname = pstrdup(me->mnt_fsname);
		m->me_mountdir = pstrdup(me->mnt_dir);
		m->me_type = pstrdup(me->mnt_type);
		m->me_dev = -1;
		m->me_dummy = is_dummy(me);
		m->me_remote = is_remote(me);
		mounts = lappend(mounts, m);
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
		FREE(m->me_type);
	}
	list_free_deep(mounts);
}

static char *resolve_dm_name(char *mapper_name)
{
	static char dm_name[NAME_MAX];
	struct dirent buf, *e;
	DIR *dir = opendir("/sys/block");

	if (dir == NULL) return mapper_name;

	while (readdir_r(dir, &buf, &e) == 0 && e != NULL)
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
						elog(ERROR, "Can't stat on %s", me->me_mountdir);
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
		return best_match->me_devname + 5;

	return best_match->me_devname; 
}

static void read_io_stats(disk_stat *stats)
{
	int i = 0;
	char device_name[255];
	unsigned long sectors_read, sectors_written;
	unsigned int time_in_queue;
	FILE *io = fopen("/proc/diskstats", "r");

	while (i < 2 && fscanf(io, "%*d %*d %s %*u %*u %lu %*u %*u %*u %lu %*u %*u %*u %u\n",
			device_name, &sectors_read, &sectors_written, &time_in_queue) == 4) {
		if (strcmp(device_name, data_dev) == 0) {
			stats->data_sectors_read = sectors_read;
			stats->data_sectors_written = sectors_written;
			stats->data_time_in_queue = time_in_queue;
			++i;
		}

		if (strcmp(device_name, xlog_dev) == 0) {
			stats->xlog_sectors_read = sectors_read;
			stats->xlog_sectors_written = sectors_written;
			stats->xlog_time_in_queue = time_in_queue;
			++i;
		}
	}
	fclose(io);
}

static void diff_disk_stats(disk_stat *new_stats)
{
	double time_diff;
	if (disk_stats_old.time.tv_sec == 0) return;
	time_diff = new_stats->time.tv_sec + new_stats->time.tv_usec/1000000.0 -
		disk_stats_old.time.tv_sec - disk_stats_old.time.tv_usec/1000000.0;

	new_stats->data_time_in_queue_diff = (new_stats->data_time_in_queue - disk_stats_old.data_time_in_queue)/time_diff;
	new_stats->xlog_time_in_queue_diff = (new_stats->xlog_time_in_queue - disk_stats_old.xlog_time_in_queue)/time_diff;

	time_diff *= 2.0; /* to obtain diffs in kB */

	new_stats->data_read_diff = (new_stats->data_sectors_read - disk_stats_old.data_sectors_read)/time_diff;
	new_stats->data_write_diff = (new_stats->data_sectors_written - disk_stats_old.data_sectors_written)/time_diff;

	new_stats->xlog_read_diff = (new_stats->xlog_sectors_read - disk_stats_old.xlog_sectors_read)/time_diff;
	new_stats->xlog_write_diff = (new_stats->xlog_sectors_written - disk_stats_old.xlog_sectors_written)/time_diff;
}

disk_stat get_diskspace_stats(void)
{
	struct timezone tz;
	struct statvfs st;
	disk_stat disk_stats = {0, };

	disk_stats.du_data = du(AT_FDCWD, DataDir, 0, &disk_stats.du_xlog);

	if (statvfs(DataDir, &st) == 0) {
		disk_stats.data_size = st.f_blocks * st.f_bsize / 1024;
		disk_stats.data_free = st.f_bavail * st.f_bsize / 1024;
	}

	if (data_dev == xlog_dev) {
		disk_stats.xlog_size = disk_stats.data_size;
		disk_stats.xlog_free = disk_stats.data_free;
	} else if (statvfs(xlog_directory, &st) == 0) {
		disk_stats.xlog_size = st.f_blocks * st.f_bsize / 1024;
		disk_stats.xlog_free = st.f_bavail * st.f_bsize / 1024;
	}

	read_io_stats(&disk_stats);

	gettimeofday(&disk_stats.time, &tz);

	diff_disk_stats(&disk_stats);

	disk_stats.data_directory = DataDir;
	disk_stats.data_dev = data_dev;

	disk_stats.xlog_directory = xlog_directory;
	disk_stats.xlog_dev = xlog_dev;

	return disk_stats_old = disk_stats;
}

void disk_stats_init(void)
{
	List *mounts = read_mounts();
	size_t len = strlen(DataDir);
	xlog_directory = palloc(len + sizeof(pg_xlog) + 2);
	strcpy(xlog_directory, DataDir);
	if (xlog_directory[len - 1] != '/')
		xlog_directory[len++] = '/';
	strcpy(xlog_directory + len, pg_xlog);

	data_dev = get_device(mounts, DataDir);
	if (data_dev) data_dev = pstrdup(data_dev);
	xlog_dev = get_device(mounts, xlog_directory);
	if (xlog_dev) {
		if (data_dev && strcmp(data_dev, xlog_dev) == 0)
			xlog_dev = data_dev;
		else xlog_dev = pstrdup(xlog_dev);
	}

	free_mounts(mounts);
}
