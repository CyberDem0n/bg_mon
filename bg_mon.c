#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>

#include "postgres.h"

/* These are always necessary for a bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"

/* these headers are used by this particular worker's code */
#include "access/xact.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"
#include "tcop/utility.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(bg_mon_launch);

#define FREE(v) do {if (v != NULL) {pfree(v); v = NULL;}} while(0)

void _PG_init(void);
void bg_mon_main(Datum);

extern char *data_directory;
extern int MaxConnections;
extern int max_worker_processes;
static unsigned long long mem_page_size;
static double SC_CLK_TCK;
extern pid_t PostmasterPid;

/* flags set by signal handlers */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* GUC variables */
static int	bg_mon_naptime = 1;

typedef struct {
	bool available;
	unsigned long long rchar;
	unsigned long long wchar;
	unsigned long long syscr;
	unsigned long long syscw;
	unsigned long long read_bytes;
	unsigned long read_bytes_diff;
	unsigned long long write_bytes;
	unsigned long write_bytes_diff;
} proc_io;

typedef struct {
	int fields;
	pid_t pid; // (1)
	char state; // (3)
	pid_t ppid; // (4)
	unsigned long utime; // (14)
	double utime_diff;
	unsigned long stime; // (15)
	double stime_diff;
	long priority; // (18)
	unsigned long long start_time; // (22)
	unsigned long vsize; // (23)
	long rss; // (24)
	unsigned long long delayacct_blkio_ticks; // (42)
	unsigned long delayacct_blkio_ticks_diff;
	unsigned long guest_time; // (43)
	double guest_time_diff;
	unsigned long long uss;
	proc_io io;
} proc_stat;

typedef struct {
	pid_t pid;
	char *datname;
	char *usename;
	int32 age;
	bool is_waiting;
	bool is_backend;
	char *locked_by;
	char *query;
	proc_stat ps;
} pg_stat;

typedef struct {
	pg_stat *values;
	size_t size;
	size_t pos;
	struct timeval time;
} pg_stat_list;

typedef struct {
	proc_stat *values;
	size_t size;
	size_t pos;
} proc_stat_list;

static pg_stat_list pg_stats_current;

static void pg_stat_list_init(pg_stat_list *list)
{
	list->pos = 0;
	list->size = MaxConnections + max_worker_processes + 17;
	list->values = palloc(sizeof(pg_stat) * list->size);
}

static void pg_stat_list_free_resources(pg_stat_list *list)
{
	size_t i;
	for (i = 0; i < list->pos; ++i) {
		pg_stat ps = list->values[i];
		if (ps.is_backend) {
			FREE(ps.query);
			FREE(ps.usename);
			FREE(ps.datname);
			FREE(ps.locked_by);
		}
	}
	list->pos = 0;
}

static bool pg_stat_list_add(pg_stat_list *list, pg_stat ps)
{
	if (list->values == NULL)
		list->pos = list->size = 0;

	if (list->pos >= list->size) {
		int new_size = list->size > 0 ? list->size * 2 : 7;
		pg_stat *nvalues = (pg_stat *)repalloc(list->values, sizeof(pg_stat)*new_size);
		if (nvalues == NULL) {
			elog(LOG, "Can't allocate %lu memory for pg_stat_list",
						sizeof(pg_stat)*new_size);
			return false;
		}
		list->size = new_size;
		list->values = nvalues;
	}
	list->values[list->pos++] = ps;
	return true;
}

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
				if (sscanf(dpos + sizeof(delimiter) - 1, "%llu", io_tab[j].value) == 1)
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
	proc_stat ps = {0,};
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

static bool proc_stats_add(proc_stat_list *list, proc_stat ps)
{
	if (list->values == NULL)
		list->pos = list->size = 0;

	if (list->pos >= list->size) {
		int new_size = list->size > 0 ? list->size * 2 : 7;
		proc_stat *nvalues = (proc_stat *)repalloc(list->values, sizeof(proc_stat)*new_size);
		if (nvalues == NULL) {
			elog(LOG, "Can't allocate %lu memory for proc_stat_list",
						sizeof(proc_stat)*new_size);
			return false;
		}
		list->size = new_size;
		list->values = nvalues;
	}
	list->values[list->pos++] = ps;
	return true;
}

static int proc_stat_cmp(const void *el1, const void *el2)
{
	return ((const proc_stat *) el1)->pid - ((const proc_stat *) el2)->pid;
}

static int pg_stat_cmp(const void *el1, const void *el2)
{
	return ((const pg_stat *) el1)->pid - ((const pg_stat *) el2)->pid;
}

static void read_procfs(pid_t ppid, proc_stat_list *list)
{
	DIR *proc;
	struct dirent *dp;
	pid_t pid;
	proc_stat ps;

	if ((proc = opendir("/proc")) == NULL) {
		elog(ERROR, "couldn't open '/proc'");
		return;
	}

	/* XXX: rewrite with readdir_r */
	while ((dp = readdir(proc)) != NULL) {
		if (dp->d_name[0] >= '1' && dp->d_name[0] <= '9'
				&& sscanf(dp->d_name, "%d", &pid) == 1
				&& pid != MyProcPid) { // skip themself
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
		elog(ERROR, "error reading /proc");

	closedir(proc);

	if (list->pos > 0)
		qsort(list->values, list->pos, sizeof(proc_stat), proc_stat_cmp);
}

static void merge_stats(pg_stat_list *pg_stats, proc_stat_list proc_stats)
{
	size_t pg_stats_pos = 0, proc_stats_pos = 0;
	size_t pg_stats_size = pg_stats->pos;

	/* Both list are sorted on pid so we need to traverse it only once */
	while (pg_stats_pos < pg_stats_size || proc_stats_pos < proc_stats.pos) {
		if (proc_stats_pos >= proc_stats.pos) { /* new backends? */
			break;
		} else if (pg_stats_pos >= pg_stats_size || // No more entries from pg_stats_activity (special process?)
				pg_stats->values[pg_stats_pos].pid > proc_stats.values[proc_stats_pos].pid) {
			pg_stat pgs = {0,};
			pgs.ps = proc_stats.values[proc_stats_pos];
			pgs.pid = pgs.ps.pid;
			/* TODO: read cmdline from proc */
			pg_stat_list_add(pg_stats, pgs);
			++proc_stats_pos;
		} else if (pg_stats->values[pg_stats_pos].pid == proc_stats.values[proc_stats_pos].pid) {
			pg_stats->values[pg_stats_pos++].ps = proc_stats.values[proc_stats_pos++];
		} else { /* pg_stats->values[pg_pos].pid < proc_stats.values[ps_pos].pid, new backend? */
			++pg_stats_pos;
		}
	}

	if (pg_stats->pos > 0)
		qsort(pg_stats->values, pg_stats->pos, sizeof(pg_stat), pg_stat_cmp);
}

static void calculate_stats_diff(pg_stat *old_stat, pg_stat *new_stat, double time_diff)
{
	proc_stat *o = &old_stat->ps;
	proc_stat *n = &new_stat->ps;
	if (n->fields < 9) return;
	n->utime_diff = (double)(n->utime - o->utime)/SC_CLK_TCK/time_diff;
	n->stime_diff = (double)(n->stime - o->stime)/SC_CLK_TCK/time_diff;
	n->guest_time_diff = (double)(n->guest_time - o->guest_time)/SC_CLK_TCK/time_diff;
	n->delayacct_blkio_ticks_diff = (n->delayacct_blkio_ticks - o->delayacct_blkio_ticks)/time_diff;
	n->io.read_bytes_diff = (n->io.read_bytes - o->io.read_bytes)/time_diff;
	n->io.write_bytes_diff = (n->io.write_bytes - o->io.write_bytes)/time_diff;
}

static void diff_pg_stats(pg_stat_list old_stats, pg_stat_list new_stats)
{
	double time_diff;
	size_t old_pos = 0, new_pos = 0;

	if (old_stats.time.tv_sec == 0) return;
	time_diff = (double)new_stats.time.tv_sec + (double)new_stats.time.tv_usec/1000000.0 -
				(double)old_stats.time.tv_sec - (double)old_stats.time.tv_usec/1000000.0;

	while (old_pos < old_stats.pos && new_pos < new_stats.pos)
		if (old_stats.values[old_pos].pid == new_stats.values[new_pos].pid)
			calculate_stats_diff(old_stats.values + old_pos++, new_stats.values + new_pos++, time_diff);
		else if (old_stats.values[old_pos].pid > new_stats.values[new_pos].pid)
			++new_pos;
		else ++old_pos; // old.pid < new.pid
}

static void report_stats()
{
	size_t i;
	for (i = 0; i < pg_stats_current.pos; ++i) {
		pg_stat s = pg_stats_current.values[i];
		proc_stat ps = s.ps;
		if (s.query != NULL)
			elog(LOG, "%d %s %c %f %f %f %lu %lu %s %s %s", s.pid, "backend", ps.state, ps.utime_diff, ps.stime_diff, ps.guest_time_diff, ps.io.read_bytes_diff, ps.io.write_bytes_diff, s.datname!=NULL?s.datname:"nil", s.usename!=NULL?s.usename:"nil", s.query);
		else if (!s.is_backend)
			elog(LOG, "%d %s %c %f %f %f %lu %lu", s.pid, "special", ps.state, ps.utime_diff, ps.stime_diff, ps.guest_time_diff, ps.io.read_bytes_diff, ps.io.write_bytes_diff);
	}
}

/******************************************************
 * implementation of du -s and du -sx functionality
 * works recursively, returns total size in kilobytes
 ******************************************************/
static unsigned long long du(int dirfd, const char *path, dev_t dev, bool track_device)
{
	struct stat st;
	unsigned long long ret = 0;
	struct dirent buf, *e;
	DIR *dir;

	if (fstatat(dirfd, path, &st, dirfd == AT_FDCWD ? 0 : AT_SYMLINK_NOFOLLOW))
		return ret;

	if (track_device && !dev) dev = st.st_dev;
	if (track_device && st.st_dev != dev) return ret;

	ret += st.st_blocks/2;

	if ((st.st_mode & S_IFMT) != S_IFDIR
			|| (dirfd = openat(dirfd, path, O_RDONLY|O_NOCTTY|O_DIRECTORY|O_NOFOLLOW)) == -1)
		return ret;

	if ((dir = fdopendir(dirfd)) == NULL) {
		close(dirfd);
		return ret;
	}

	while (readdir_r(dir, &buf, &e) == 0 && e != NULL)
		// skip "." and ".."
		if (e->d_name[0] != '.'
				|| (e->d_name[1] && (e->d_name[1] != '.' || e->d_name[2]))
				|| !strncmp(e->d_name, "lost+found", 11))
			ret += du(dirfd, e->d_name, dev, track_device);

	closedir(dir); // close dirfd as well
	return ret;
}

/*
 * Signal handler for SIGTERM
 *		Set a flag to let the main loop to terminate, and set our latch to wake
 *		it up.
 */
static void
bg_mon_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sigterm = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

/*
 * Signal handler for SIGHUP
 *		Set a flag to tell the main loop to reread the config file, and set
 *		our latch to wake it up.
 */
static void
bg_mon_sighup(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sighup = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

/*
 * Initialize workspace for a worker process: create the schema if it doesn't
 * already exist.
 */
static void
initialize_bg_mon()
{
	mem_page_size = getpagesize();
	SC_CLK_TCK = sysconf(_SC_CLK_TCK);
	pg_stat_list_init(&pg_stats_current);
	elog(LOG, "data_directory=%s max_connections=%d, max_worker_processes=%d", data_directory, MaxConnections, max_worker_processes);
}

void
bg_mon_main(Datum main_arg)
{
	struct timezone tz;
	struct timeval current_time, next_run;
	pg_stat_list pg_stats, tmp;
	proc_stat_list proc_stats;
	StringInfoData buf;

	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, bg_mon_sighup);
	pqsignal(SIGTERM, bg_mon_sigterm);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
	BackgroundWorkerInitializeConnection("postgres", NULL);

	initialize_bg_mon();

	pg_stat_list_init(&pg_stats);

	proc_stats.values = palloc(sizeof(proc_stat) * (proc_stats.size = pg_stats.size));

	/*
	 * Quote identifiers passed to us.  Note that this must be done after
	 * initialize_bg_mon, because that routine assumes the names are not
	 * quoted.
	 *
	 * Note some memory might be leaked here.
	 */

	initStringInfo(&buf);
	appendStringInfo(&buf,
					"WITH activity AS ("
					"SELECT datname,"
							"a.pid as pid,"
							"usename,"
							"client_addr,"
							"client_port,"
							"round(extract(epoch from (now() - xact_start))) as age,"
							"waiting,"
							"string_agg(other.pid::TEXT, ',' ORDER BY other.pid) as locked_by,"
							"CASE "
								"WHEN state = 'idle in transaction' THEN "
									"CASE WHEN xact_start != state_change THEN "
										"state||' '||CAST( abs(round(extract(epoch from (now() - state_change)))) AS text ) "
									"ELSE "
										"state "
									"END "
								"WHEN state = 'active' THEN query "
								"ELSE state "
								"END AS query "
					"FROM pg_stat_activity a "
					"LEFT JOIN pg_locks  this ON (this.pid = a.pid and this.granted = 'f') "
/*					-- acquire the same type of lock that is granted */
					"LEFT JOIN pg_locks other ON ((this.locktype = other.locktype AND other.granted = 't') "
												"AND ( ( this.locktype IN ('relation', 'extend') "
														"AND this.database = other.database "
														"AND this.relation = other.relation) "
													"OR (this.locktype ='page' "
														"AND this.database = other.database "
														"AND this.relation = other.relation "
														"AND this.page = other.page) "
													"OR (this.locktype ='tuple' "
														"AND this.database = other.database "
														"AND this.relation = other.relation "
														"AND this.page = other.page "
														"AND this.tuple = other.tuple) "
													"OR (this.locktype ='transactionid' "
														"AND this.transactionid = other.transactionid) "
													"OR (this.locktype = 'virtualxid' "
														"AND this.virtualxid = other.virtualxid) "
													"OR (this.locktype IN ('object', 'userlock', 'advisory') "
														"AND this.database = other.database "
														"AND this.classid = other.classid "
														"AND this.objid = other.objid "
														"AND this.objsubid = other.objsubid))"
													") "
					"WHERE a.pid != pg_backend_pid() "
					"GROUP BY 1,2,3,4,5,6,7,9 "
					"ORDER BY 2"
					") SELECT *,"
					"pid IN (SELECT DISTINCT((string_to_array(locked_by, ','))[1]::integer) "
							"FROM activity WHERE locked_by IS NOT NULL AND locked_by <> '') AS is_locker"
					" FROM activity");

	gettimeofday(&next_run, &tz);

	/*
	 * Main loop: do this until the SIGTERM handler tells us to terminate
	 */
	while (!got_sigterm)
	{
		MemoryContext oldcxt, uppercxt = CurrentMemoryContext;
		int			ret;
		int			rc;
		long double	naptime = bg_mon_naptime * 1000L;

		next_run.tv_sec += bg_mon_naptime;
		gettimeofday(&current_time, &tz);

		naptime = 1000L * (
			(double)next_run.tv_sec + (double)next_run.tv_usec/1000000.0 -
			(double)current_time.tv_sec - (double)current_time.tv_usec/1000000.0
		);

		if (naptime <= 0) { // something is very slow
			next_run = current_time; // reschedule next run
		} else {
			/*
			 * Background workers mustn't call usleep() or any direct equivalent:
			 * instead, they may wait on their process latch, which sleeps as
			 * necessary, but is awakened if postmaster dies.  That way the
			 * background process goes away immediately in an emergency.
			 */
			rc = WaitLatch(&MyProc->procLatch,
						   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
						   naptime);

			ResetLatch(&MyProc->procLatch);

			/* emergency bailout if postmaster has died */
			if (rc & WL_POSTMASTER_DEATH)
				proc_exit(1);
		}

		/*
		 * In case of a SIGHUP, just reload the configuration.
		 */
		if (got_sighup)
		{
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		proc_stats.pos = 0;
		read_procfs(PostmasterPid, &proc_stats);

		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());
		pgstat_report_activity(STATE_RUNNING, buf.data);


		/* We can now execute queries via SPI */
		ret = SPI_execute(buf.data, true, 0);

		if (ret != SPI_OK_SELECT)
			elog(FATAL, "cannot select from pg_stat_activity: error code %d", ret);

		if (SPI_processed > 0)
		{
			int a;
			bool isnull, is_locker;

			oldcxt = MemoryContextSwitchTo(uppercxt);
			for (a = 0; a < SPI_processed; a++) {
				pg_stat ps = {0, };

				ps.pid = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 2, &isnull)); 
				is_locker = DatumGetBool(SPI_getbinval(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 10, &isnull));
				ps.query = SPI_getvalue(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 9);

				if (is_locker || (ps.query != NULL && strncmp(ps.query, "idle", 5))) {
					ps.datname = SPI_getvalue(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 1);
					ps.usename = SPI_getvalue(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 3);
					ps.age = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 6, &isnull));
					if (isnull) ps.age = -1;
					ps.is_waiting = DatumGetBool(SPI_getbinval(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 7, &isnull)); 
					ps.locked_by = SPI_getvalue(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 8);
				} else FREE(ps.query);

				ps.is_backend = true;
				pg_stat_list_add(&pg_stats, ps);
			}

			MemoryContextSwitchTo(oldcxt);
		}

		/*
		 * And finish our transaction.
		 */
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		pgstat_report_activity(STATE_IDLE, NULL);

		merge_stats(&pg_stats, proc_stats);
		gettimeofday(&pg_stats.time, &tz);
		diff_pg_stats(pg_stats_current, pg_stats);

		tmp = pg_stats_current;
		pg_stats_current = pg_stats;
		pg_stat_list_free_resources(&tmp);
		pg_stats = tmp;
		du(AT_FDCWD, data_directory, 0, true);
		report_stats();

	}

	proc_exit(1);
}

/*
 * Entrypoint of this module.
 *
 * We register more than one worker process here, to demonstrate how that can
 * be done.
 */
void
_PG_init(void)
{
	BackgroundWorker worker;
	unsigned int i;

	/* get the configuration */
	DefineCustomIntVariable("bg_mon.naptime",
							"Duration between each run (in seconds).",
							NULL,
							&bg_mon_naptime,
							1,
							1,
							10,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	if (!process_shared_preload_libraries_in_progress)
		return;

	/* set up common data for all our workers */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main = bg_mon_main;
	worker.bgw_notify_pid = 0;

	/*
	 * Now fill in worker-specific data, and do the actual registrations.
	 */
	i = 1;
	snprintf(worker.bgw_name, BGW_MAXLEN, "worker %d", i);
	worker.bgw_main_arg = Int32GetDatum(i);

	RegisterBackgroundWorker(&worker);
}

/*
 * Dynamically launch an SPI worker.
 */
Datum
bg_mon_launch(PG_FUNCTION_ARGS)
{
	int32		i = 1;
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	BgwHandleStatus status;
	pid_t		pid;

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main = NULL;		/* new worker might not have library loaded */
	sprintf(worker.bgw_library_name, "bg_mon");
	sprintf(worker.bgw_function_name, "bg_mon_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "worker %d", i);
	worker.bgw_main_arg = Int32GetDatum(i);
	/* set bgw_notify_pid so that we can use WaitForBackgroundWorkerStartup */
	worker.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		PG_RETURN_NULL();

	status = WaitForBackgroundWorkerStartup(handle, &pid);

	if (status == BGWH_STOPPED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not start background process"),
			   errhint("More details may be available in the server log.")));
	if (status == BGWH_POSTMASTER_DIED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
			  errmsg("cannot start background processes without postmaster"),
				 errhint("Kill all remaining database processes and restart the database.")));
	Assert(status == BGWH_STARTED);

	PG_RETURN_INT32(pid);
}
