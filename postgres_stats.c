#include <unistd.h>
#include <dirent.h>

#include "postgres.h"

#include "miscadmin.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "pgstat.h"
#include "utils/snapmgr.h"

#include "postgres_stats.h"

#define FREE(v) do {if (v != NULL) {pfree(v); v = NULL;}} while(0)

extern pid_t PostmasterPid;
extern int MaxBackends;

static unsigned long long mem_page_size;
static double SC_CLK_TCK;
static StringInfoData pg_stat_activity_query;

typedef struct {
	proc_stat *values;
	size_t size;
	size_t pos;
} proc_stat_list;

static proc_stat_list proc_stats;
static pg_stat_list pg_stats_current;
static pg_stat_list pg_stats_new;

static void pg_stat_list_init(pg_stat_list *list)
{
	list->pos = 0;
	list->size = MaxBackends + 17;
	list->values = palloc(sizeof(pg_stat) * list->size);
}

static void pg_stat_list_free_resources(pg_stat_list *list)
{
	size_t i;
	for (i = 0; i < list->pos; ++i) {
		pg_stat ps = list->values[i];
		FREE(ps.query);
		if (ps.is_backend) {
			FREE(ps.usename);
			FREE(ps.datname);
			FREE(ps.locked_by);
		}

		if (ps.ps.free_cmdline)
			FREE(ps.ps.cmdline);
	}
	list->pos = 0;
}

static bool pg_stat_list_add(pg_stat_list *list, pg_stat ps)
{
	if (list->values == NULL)
		list->pos = list->size = 0;

	if (list->pos >= list->size) {
		int new_size = list->size > 0 ? list->size * 2 : 7;
		list->size = new_size;
		list->values = repalloc(list->values, sizeof(pg_stat)*new_size);
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
	int i = 0, j = 0;
	proc_io pi = {0,};
	char *dpos, buf[255];
	const char delimiter[] = ": ";
	struct _io_tab {
		const char *name;
		unsigned long long *value;
	} io_tab[] = {
		{"read_bytes", &pi.read_bytes},
		{"write_bytes", &pi.write_bytes},
		{NULL, NULL}
	};

	FILE *iofd = open_proc_file(pid, "io");

	if (iofd == NULL)
		return pi;

	while (fgets(buf, sizeof(buf), iofd) && i < sizeof(io_tab)/sizeof(struct _io_tab) - 1) {
		if ((dpos = strstr(buf, delimiter)) == NULL)
			continue;

		for (j = 0; io_tab[j].name != NULL; ++j)
			if (strncmp(io_tab[j].name, buf, dpos - buf) == 0) {
				++i;
				if (sscanf(dpos + sizeof(delimiter) - 1, "%llu", io_tab[j].value) == 1)
					pi.available = true;
				break;
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
		elog(DEBUG1, "Can't parse content of /proc/%d/stat", pid);
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
		list->size = new_size;
		list->values = repalloc(list->values, sizeof(proc_stat)*new_size);
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
	struct dirent buf, *dp;
	pid_t pid;
	proc_stat ps;

	list->pos = 0;

	pgstat_report_activity(STATE_RUNNING, "collecting statistics from procfs");

	if ((proc = opendir("/proc")) == NULL) {
		elog(ERROR, "couldn't open '/proc'");
		return;
	}

	while (readdir_r(proc, &buf, &dp) == 0 && dp != NULL) {
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

static void read_proc_cmdline(pg_stat *stat)
{
	char buf[MAXPGPATH];
	FILE *f = open_proc_file(stat->pid, "cmdline");
	if (f == NULL) return;

	if (fgets(buf, sizeof(buf), f) && strncmp(buf, "postgres: ", 10) == 0) {
		char *p, *lp = NULL;

		// cluster name can contain ' process ' string, so we need to skip all but last
		for (p = buf + 1; (p = strstr(p + 9, " process ")) != NULL; lp = p);

		if (lp != NULL) {
			for (p = lp + 9; *p == ' '; p++);
			if (*p) stat->query = pstrdup(p);
		}

		if (stat->ps.cmdline == NULL) {
			if (lp == NULL) { // no " process " delimiter string in the cmdline
				// it could be a bgworker process
				// cluster name can contain ' bgworker: ' string, so we need to skip all but last
				for (p = buf - 1; (p = strstr(p + 11, " bgworker: ")) != NULL; lp = p);
				if (lp == NULL) stat->ps.cmdline = pstrdup("unknown");
				else {
					for (p = lp + 11; *p != '\0'; ++p);
					while (*(--p) == ' ') *p = '\0'; // trim whitespaces
					stat->ps.cmdline = pstrdup(lp + 1);
				}
			} else {
				*lp = '\0';
				// cmdline starts after last colon followed by whitespace
				for (p = lp - 1; *p != ':'; --p);
				stat->ps.cmdline = pstrdup(p + 2);
			}
		}
	}
	fclose(f);
}

static void calculate_stats_diff(pg_stat *old_stat, pg_stat *new_stat, double time_diff)
{
	proc_stat *o = &old_stat->ps;
	proc_stat *n = &new_stat->ps;
	if (n->fields < 9) return;

	n->delayacct_blkio_ticks_diff = (n->delayacct_blkio_ticks - o->delayacct_blkio_ticks)/time_diff;
	n->io.read_diff = (n->io.read_bytes - o->io.read_bytes)/time_diff/1024;
	n->io.write_diff = (n->io.write_bytes - o->io.write_bytes)/time_diff/1024;

	time_diff *= SC_CLK_TCK;

	n->utime_diff = (n->utime - o->utime)/time_diff;
	n->stime_diff = (n->stime - o->stime)/time_diff;
	n->guest_time_diff = (n->guest_time - o->guest_time)/time_diff;
}

static void diff_pg_stats(pg_stat_list old_stats, pg_stat_list new_stats)
{
	double time_diff;
	size_t old_pos = 0, new_pos = 0;

	if (old_stats.time.tv_sec == 0) return;
	time_diff = new_stats.time.tv_sec + new_stats.time.tv_usec/1000000.0 -
				old_stats.time.tv_sec - old_stats.time.tv_usec/1000000.0;

	while (old_pos < old_stats.pos || new_pos < new_stats.pos) {
		if (new_pos >= new_stats.pos)
			old_stats.values[old_pos++].ps.free_cmdline = true;
		else if (old_pos >= old_stats.pos) {
			if (!new_stats.values[new_pos].is_backend)
				read_proc_cmdline(new_stats.values + new_pos);
			++new_pos;
		} else if (old_stats.values[old_pos].pid == new_stats.values[new_pos].pid) {
			if (new_stats.values[new_pos].is_backend)
				old_stats.values[old_pos].ps.free_cmdline = true;
			else {
				if (old_stats.values[old_pos].is_backend)
					old_stats.values[old_pos].ps.free_cmdline = true;

				if (old_stats.values[old_pos].ps.cmdline != NULL)
					new_stats.values[new_pos].ps.cmdline = old_stats.values[old_pos].ps.cmdline;
				read_proc_cmdline(new_stats.values + new_pos);
			}

			calculate_stats_diff(old_stats.values + old_pos++, new_stats.values + new_pos++, time_diff);
		} else if (old_stats.values[old_pos].pid > new_stats.values[new_pos].pid) {
			if (!new_stats.values[new_pos].is_backend)
				read_proc_cmdline(new_stats.values + new_pos);
			++new_pos;
		} else // old.pid < new.pid
			old_stats.values[old_pos++].ps.free_cmdline = true;
	}
}

static void get_pg_stat_activity(pg_stat_list *pg_stats)
{
	struct timezone tz;
	int ret;
	MemoryContext uppercxt = CurrentMemoryContext;

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());
	pgstat_report_activity(STATE_RUNNING, pg_stat_activity_query.data);

	/* We can now execute queries via SPI */
	ret = SPI_execute(pg_stat_activity_query.data, true, 0);

	if (ret != SPI_OK_SELECT)
		elog(FATAL, "cannot select from pg_stat_activity: error code %d", ret);

	pg_stats->recovery_in_progress = RecoveryInProgress();
	pg_stats->total_connections = SPI_processed + 1;
	pg_stats->active_connections = 0;

	if (SPI_processed > 0)
	{
		int a;
		bool isnull, is_locker;

		MemoryContext oldcxt = MemoryContextSwitchTo(uppercxt);
		for (a = 0; a < SPI_processed; a++) {
			pg_stat ps = {0, };

			ps.pid = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 2, &isnull)); 
			is_locker = DatumGetBool(SPI_getbinval(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 10, &isnull));
			ps.query = SPI_getvalue(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 9);

			if (is_locker || (ps.query != NULL && strncmp(ps.query, "\"idle\"", 7))) {
				ps.datname = SPI_getvalue(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 1);
				ps.usename = SPI_getvalue(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 3);
				ps.age = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 6, &isnull));
				if (isnull) ps.age = -1;
				ps.is_waiting = DatumGetBool(SPI_getbinval(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 7, &isnull)); 
				ps.locked_by = SPI_getvalue(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 8);
				pg_stats->active_connections++;
			} else FREE(ps.query);

			ps.is_backend = true;
			pg_stat_list_add(pg_stats, ps);
		}

		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * And finish our transaction.
	 */
	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();
	gettimeofday(&pg_stats->time, &tz);
}

pg_stat_list get_postgres_stats(void)
{
	pg_stat_list pg_stats_tmp;

	read_procfs(PostmasterPid, &proc_stats);

	pg_stat_list_free_resources(&pg_stats_new);
	get_pg_stat_activity(&pg_stats_new);

	pgstat_report_activity(STATE_IDLE, NULL);

	merge_stats(&pg_stats_new, proc_stats);
	diff_pg_stats(pg_stats_current, pg_stats_new);

	pg_stats_tmp = pg_stats_current;
	pg_stats_current = pg_stats_new;
	pg_stats_new = pg_stats_tmp;

	return pg_stats_current;
}

void postgres_stats_init(void)
{
	mem_page_size = getpagesize();
	SC_CLK_TCK = sysconf(_SC_CLK_TCK);
	pg_stat_list_init(&pg_stats_current);
	pg_stat_list_init(&pg_stats_new);

	proc_stats.values = palloc(sizeof(proc_stat) * (proc_stats.size = pg_stats_current.size));

	initStringInfo(&pg_stat_activity_query);
	appendStringInfo(&pg_stat_activity_query,
					"WITH activity AS ("
						"SELECT to_json(datname)::text AS datname,"
								"a.pid as pid,"
								"to_json(usename)::text AS usename,"
								"client_addr,"
								"client_port,"
								"round(extract(epoch from (now() - xact_start)))::int as age,"
#if PG_VERSION_NUM >= 90600
								"wait_event IS NOT NULL as "
#endif
								"waiting,"
								"to_json(CASE WHEN state = 'idle in transaction' THEN "
										"CASE WHEN xact_start != state_change THEN "
											"'idle in transaction ' || CAST("
												"abs(round(extract(epoch from (now() - state_change)))) AS text) "
											"ELSE state "
										"END "
									"WHEN state = 'active' THEN query "
									"ELSE state "
								"END)::text AS query,"
#if PG_VERSION_NUM >= 90600
								"ARRAY(SELECT unnest(pg_blocking_pids(a.pid)) ORDER BY 1) as locked_by "
#else
								"array_agg(distinct other.pid ORDER BY other.pid) as locked_by "
#endif
						"FROM pg_stat_activity a "
#if PG_VERSION_NUM < 90600
						"LEFT JOIN pg_locks  this ON this.pid = a.pid and this.granted = 'f' "
						"LEFT JOIN pg_locks other ON this.locktype = other.locktype "
													"AND this.database IS NOT DISTINCT FROM other.database "
													"AND this.relation IS NOT DISTINCT FROM other.relation "
													"AND this.page IS NOT DISTINCT FROM other.page "
													"AND this.tuple IS NOT DISTINCT FROM other.tuple "
													"AND this.virtualxid IS NOT DISTINCT FROM other.virtualxid "
													"AND this.transactionid IS NOT DISTINCT FROM other.transactionid "
													"AND this.classid IS NOT DISTINCT FROM other.classid "
													"AND this.objid IS NOT DISTINCT FROM other.objid "
													"AND this.objsubid IS NOT DISTINCT FROM other.objsubid "
													"AND this.pid != other.pid "
													"AND other.granted = 't' "
#endif
						"WHERE a.pid != pg_backend_pid() "
						"GROUP BY 1,2,3,4,5,6,7,8"
					"), lockers AS ("
						"SELECT DISTINCT(locked_by[1])"
						" FROM activity "
						"WHERE locked_by IS NOT NULL"
					") SELECT datname, pid, usename, client_addr, client_port, age, waiting,"
							" NULLIF(array_to_string(locked_by, ','), ''), query, pid IN (SELECT * FROM lockers)"
					" FROM activity"
					" ORDER BY 2");
}
