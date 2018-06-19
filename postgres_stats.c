#include <unistd.h>
#include <dirent.h>

#include "postgres.h"

#include "miscadmin.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "pgstat.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"

#include "system_stats.h"
#include "postgres_stats.h"

extern system_stat system_stats_old;

extern pid_t PostmasterPid;
extern int MaxBackends;

static size_t cmdline_prefix_len;
static char *cmdline_prefix;
static size_t postmaster_pid_len;
static char postmaster_pid[32];
static unsigned long long mem_page_size;
static SPIPlanPtr pg_stat_activity_query_plan;
static const char * const pg_stat_activity_query =
"WITH locked_processes AS ("
	"SELECT this.pid as pid, "
#if PG_VERSION_NUM >= 90600
			"ARRAY(SELECT unnest(pg_blocking_pids(this.pid)) ORDER BY 1) AS locked_by"
#else
			"array_agg(DISTINCT other.pid ORDER BY other.pid) AS locked_by"
#endif
	" FROM pg_locks this"
#if PG_VERSION_NUM < 90600
	" JOIN pg_locks other ON this.locktype = other.locktype "
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
						"AND other.granted"
#endif
	" WHERE NOT this.granted"
#if PG_VERSION_NUM < 90600
	" GROUP BY 1"
#endif
"), lockers AS ("
	"SELECT DISTINCT unnest(locked_by)"
	" FROM locked_processes"
") SELECT pid,"
		" datname::text,"
		" usename::text,"
		" round(extract(epoch from (now() - COALESCE(xact_start, CASE WHEN state = 'active'"
																	" THEN query_start"
																	" ELSE NULL END))))::int AS age,"
		" NULLIF(array_to_string(locked_by, ','), ''),"
		" CASE WHEN state = 'idle in transaction' THEN"
				" CASE WHEN xact_start != state_change THEN"
					" 'idle in transaction ' || CAST("
						" abs(round(extract(epoch from (now() - state_change)))) AS text)"
				" ELSE state END"
		" WHEN state = 'active' THEN query"
		" ELSE state END::text AS query,"
		" pid IN (SELECT * FROM lockers)"
#if PG_VERSION_NUM >= 100000
		", CASE backend_type WHEN 'autovacuum worker' THEN 1 WHEN 'walsender' THEN 8 WHEN 'client backend' THEN 2 ELSE -2 END"
#endif
	" FROM pg_stat_activity a"
	" LEFT JOIN locked_processes USING (pid) "
	"WHERE pid != pg_backend_pid()"
#if PG_VERSION_NUM >= 100000
	" AND backend_type IN ('client backend', 'autovacuum worker', 'walsender', 'background worker')"
#endif
;

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
		FREE(ps.usename);
		FREE(ps.datname);
		FREE(ps.locked_by);

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


static size_t json_escaped_size(const char *s, size_t len)
{
	size_t ret = 0;
	while (len-- > 0) {
		switch (*s) {
			case 0x00:
				return ret;
			case '"':
			case '\\':
			case '\b':
			case '\f':
			case '\n':
			case '\r':
			case '\t':
				ret += 1; /* from 1 byte to \x 2 bytes */
				break;
			case 0x01:
			case 0x02:
			case 0x03:
			case 0x04:
			case 0x05:
			case 0x06:
			case 0x07:
			case 0x0b:
			case 0x0e:
			case 0x0f:
			case 0x10:
			case 0x11:
			case 0x12:
			case 0x13:
			case 0x14:
			case 0x15:
			case 0x16:
			case 0x17:
			case 0x18:
			case 0x19:
			case 0x1a:
			case 0x1b:
			case 0x1c:
			case 0x1d:
			case 0x1e:
			case 0x1f:
				ret += 5; /* from 1 byte to \uxxxx 6 bytes */
				break;
		}
		++s;
		++ret;
	}
	return ret;
}

static char *json_escape_string_len(const char *s, size_t len)
{
	char *ret = palloc(json_escaped_size(s, len) + 3);
	char *r = ret;
	*(r++) = '"';

	while (len-- > 0) {
		switch (*s) {
			case 0x00:
				*(r++) = '"';
				*r = '\0';
				return ret;
			case '"':
				*(r++) = '\\';
				*r = '"';
				break;
			case '\\':
				*(r++) = '\\';
				*r = '\\';
				break;
			case '\b':
				*(r++) = '\\';
				*r = 'b';
				break;
			case '\f':
				*(r++) = '\\';
				*r = 'f';
				break;
			case '\n':
				*(r++) = '\\';
				*r = 'n';
				break;
			case '\r':
				*(r++) = '\\';
				*r = 'r';
				break;
			case '\t':
				*(r++) = '\\';
				*r = 't';
				break;
			case 0x01:
			case 0x02:
			case 0x03:
			case 0x04:
			case 0x05:
			case 0x06:
			case 0x07:
			case 0x0b:
			case 0x0e:
			case 0x0f:
			case 0x10:
			case 0x11:
			case 0x12:
			case 0x13:
			case 0x14:
			case 0x15:
			case 0x16:
			case 0x17:
			case 0x18:
			case 0x19:
			case 0x1a:
			case 0x1b:
			case 0x1c:
			case 0x1d:
			case 0x1e:
			case 0x1f:
			{
				// convert a number 0..15 to its hex representation (0..f)
				static const char hexify[16] = {
					'0', '1', '2', '3', '4', '5', '6', '7',
					'8', '9', 'a', 'b', 'c', 'd', 'e', 'f'
				};

				// print character c as \uxxxx
				*(r++) = '\\';
				*(r++) = 'u';
				*(r++) = '0';
				*(r++) = '0';
				*(r++) = hexify[*s >> 4];
				*r = hexify[*s & 0x0f];
				break;
			}
			default:
				*r = *s;
				break;
		}
		++r;
		++s;
	}
	*(r++) = '"';
	*r = '\0';
	return ret;
}

static char *json_escape_string(const char *s)
{
	return json_escape_string_len(s, -1);
}

static unsigned long long get_memory_usage(const char *proc_file)
{
	char *p, *endptr, buf[255];
	unsigned long resident = 0, share = 0;
	FILE *fd = fopen(proc_file, "r");
	if (fd == NULL) return 0;
	if (fgets(buf, sizeof(buf), fd)) {
		for (p = buf; *p != ' ' && *p != '\0'; ++p);
		if (*p == ' ') {
			resident = strtoul(p + 1, &endptr, 10);
			if (endptr > p + 1 && *endptr == ' ')
				share = strtoul(endptr + 1, NULL, 10);
		}
	}
	fclose(fd);
	return (unsigned long long)(resident - share) * mem_page_size;
}

static proc_io read_proc_io(const char *proc_file)
{
	int i = 0, j = 0;
	proc_io pi = {0,};
	char *dpos, *endptr, buf[255];
	struct _io_tab {
		const char *name;
		size_t name_len;
		unsigned long long *value;
	} io_tab[] = {
		{"read_bytes: ", 12, &pi.read_bytes},
		{"write_bytes: ", 13, &pi.write_bytes},
		{"cancelled_write_bytes: ", 23, &pi.cancelled_write_bytes},
		{NULL, 0, NULL}
	};

	FILE *iofd = fopen(proc_file, "r");

	if (iofd == NULL)
		return pi;

	while (fgets(buf, sizeof(buf), iofd) && i < sizeof(io_tab)/sizeof(struct _io_tab) - 1) {
		for (j = 0; io_tab[j].name != NULL; ++j)
			if (strncmp(io_tab[j].name, buf, io_tab[j].name_len) == 0) {
				++i;
				dpos = buf + io_tab[j].name_len;
				*io_tab[j].value = strtoull(dpos, &endptr, 10);
				if (endptr > dpos)
				pi.available = true;
				break;
			}
	}

	fclose(iofd);
	return pi;
}

static proc_stat read_proc_stat(const char *proc_file)
{
	char *t, buf[512];
	proc_stat ps = {0,};
	FILE *statfd = fopen(proc_file, "r");

	if (statfd == NULL)
		return ps;

	if (fgets(buf, sizeof(buf), statfd)) {
		for (t = buf; *t != ')' && *t != '\0'; ++t);
		if (*t == ')' && strncmp(t + 3, postmaster_pid, postmaster_pid_len) == 0) {
			ps.fields = sscanf(t + 2, "%c %d %*d %*d %*d %*d %*u %*u %*u \
		%*u %*u %lu %lu %*d %*d %ld %*d %*d %*d %llu %lu %ld %*u %*u %*u %*u %*u %*u \
		%*u %*u %*u %*u %*u %*u %*u %*d %*d %*u %*u %llu %lu", &ps.state,
				&ps.ppid, &ps.utime, &ps.stime, &ps.priority, &ps.start_time, &ps.vsize,
				&ps.rss, &ps.delayacct_blkio_ticks, &ps.gtime);
			if (ps.fields < 8) {
				elog(DEBUG1, "Can't parse content of %s", proc_file);
				ps.fields = ps.ppid = 0;
			}
		}
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

static void read_procfs(proc_stat_list *list)
{
	DIR *proc;
	struct dirent *dp = NULL;
	pid_t pid;
	char *endptr;
	char proc_file[32] = "/proc";
	proc_stat ps;

	list->pos = 0;

	pgstat_report_activity(STATE_RUNNING, "collecting statistics from procfs");

	if ((proc = opendir(proc_file)) == NULL) {
		elog(ERROR, "couldn't open '/proc'");
		return;
	}

	while (errno = 0, NULL != (dp = readdir(proc))) {
		if (dp->d_name[0] >= '1' && dp->d_name[0] <= '9'
				&& (pid = strtoul(dp->d_name, &endptr, 10)) > 0
				&& endptr > dp->d_name && *endptr == '\0'
				&& pid != MyProcPid) { // skip themself
					size_t pid_len = endptr - dp->d_name;
					proc_file[5] = '/';
					memcpy(proc_file + 6, dp->d_name, pid_len);
					proc_file[pid_len + 6] = '/';
					strcpy(proc_file + pid_len + 7, "stat");
			ps = read_proc_stat(proc_file);
			if (ps.fields && ps.ppid == PostmasterPid) {
				ps.pid = pid;
				proc_file[pid_len + 11] = 'm'; // statm
				proc_file[pid_len + 12] = '\0';
				ps.uss = get_memory_usage(proc_file);
				strcpy(proc_file + pid_len + 7, "io");
				ps.io = read_proc_io(proc_file);
				proc_stats_add(list, ps);
			}
		}
	}

	if (dp == NULL && errno != 0)
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
			pgs.type = PG_UNDEFINED; /* unknown process */
			pg_stat_list_add(pg_stats, pgs);
			++proc_stats_pos;
		} else if (pg_stats->values[pg_stats_pos].pid == proc_stats.values[proc_stats_pos].pid) {
			pg_stats->values[pg_stats_pos++].ps = proc_stats.values[proc_stats_pos++];
		} else { /* pg_stats->values[pg_pos].pid < proc_stats.values[ps_pos].pid, new backend? */
			pg_stats->values[pg_stats_pos++].ps.state = 'S';
		}
	}

	if (pg_stats->pos > 0)
		qsort(pg_stats->values, pg_stats->pos, sizeof(pg_stat), pg_stat_cmp);
}

static PgBackendType parse_cmdline(const char * const buf, const char **rest)
{
	PgBackendType type = PG_UNDEFINED;
	*rest = buf;
	if (strncmp(buf, cmdline_prefix, cmdline_prefix_len) == 0) {
		int j;
		const char * const cmd = buf + cmdline_prefix_len;
		struct _backend_tab {
			const char *name;
			size_t name_len;
			PgBackendType type;
		} backend_tab[] = {
#if PG_VERSION_NUM < 110000
			{"archiver process   ", 19, PG_ARCHIVER},
			{"startup process   ", 18, PG_STARTUP},
			{"wal receiver process   ", 23, PG_WAL_RECEIVER},
			{"wal sender process ", 19, PG_WAL_SENDER},
			{"autovacuum launcher process   ", 30, PG_AUTOVAC_LAUNCHER},
			{"autovacuum worker process   ", 28, PG_AUTOVAC_WORKER},
			{"bgworker: logical replication launcher   ", 41, PG_LOGICAL_LAUNCHER},
			{"bgworker: logical replication worker for ", 41, PG_LOGICAL_WORKER},
			{"bgworker: ", 10, PG_BG_WORKER},
			{"checkpointer process   ", 23, PG_CHECKPOINTER},
			{"logger process   ", 17, PG_LOGGER},
			{"stats collector process   ", 26, PG_STATS_COLLECTOR},
			{"wal writer process   ", 21, PG_WAL_WRITER},
			{"writer process   ", 17, PG_BG_WRITER},
#else
			{"archiver   ", 11, PG_ARCHIVER},
			{"startup   ", 10, PG_STARTUP},
			{"walreceiver   ", 14, PG_WAL_RECEIVER},
			{"walsender ", 10, PG_WAL_SENDER},
			{"autovacuum launcher   ", 22, PG_AUTOVAC_LAUNCHER},
			{"autovacuum worker   ", 20, PG_AUTOVAC_WORKER},
			{"logical replication launcher   ", 31, PG_LOGICAL_LAUNCHER},
			{"logical replication worker for ", 31, PG_LOGICAL_WORKER},
			{"background writer   ", 20, PG_BG_WRITER},
			{"checkpointer   ", 15, PG_CHECKPOINTER},
			{"logger   ", 9, PG_LOGGER},
			{"stats collector   ", 18, PG_STATS_COLLECTOR},
			{"walwriter   ", 12, PG_WAL_WRITER},

#endif
			{"??? process   ", 14, PG_UNKNOWN},
			{NULL, 0, 0}
		};

		for (j = 0; backend_tab[j].name != NULL; ++j)
			if (strncmp(cmd, backend_tab[j].name, backend_tab[j].name_len) == 0) {
				type = backend_tab[j].type;
				*rest = cmd + backend_tab[j].name_len;
				break;
			}

#if PG_VERSION_NUM >= 110000
		if (backend_tab[j].name == NULL) {
			size_t len = strlen(cmd);
			if (len > 3 && strcmp(cmd + len - 3, "   ") == 0) {
				type = PG_BG_WORKER;
				*rest = cmd;
			}
		}
#endif
	}
	return type;
}

static void read_proc_cmdline(pg_stat *stat)
{
	FILE *f;
	char buf[MAXPGPATH];
	char proc_file[32];

	if (stat->type != PG_UNDEFINED && stat->type != PG_ARCHIVER
			&& stat->type != PG_STARTUP && stat->type != PG_WAL_RECEIVER
			&& (stat->type != PG_WAL_SENDER || stat->query))
		return;

	sprintf(proc_file, "/proc/%d/cmdline", stat->pid);

	if ((f = fopen(proc_file, "r")) == NULL) return;

	if (fgets(buf, sizeof(buf), f)) {
		const char *rest;
		PgBackendType type = parse_cmdline(buf, &rest);

		stat->type = type;
		if ((type == PG_WAL_RECEIVER || type == PG_WAL_SENDER
				|| type == PG_ARCHIVER || type == PG_STARTUP) && *rest) {
#if PG_VERSION_NUM >= 100000
			if (type == PG_WAL_SENDER && stat->usename) {
				size_t len = strlen(stat->usename) - 2;
				if (strncmp(rest, stat->usename + 1, len) == 0 && rest[len] == ' ')
					rest += len + 1;
			}
#endif
			stat->query = json_escape_string(rest);
		} else if ((type == PG_LOGICAL_WORKER || type == PG_BG_WORKER) && *rest)
			stat->ps.cmdline = json_escape_string_len(rest, strlen(rest) - 3);
	}
	fclose(f);
}

static void calculate_stats_diff(pg_stat *old_stat, pg_stat *new_stat, unsigned long long itv)
{
	proc_stat *o = &old_stat->ps;
	proc_stat *n = &new_stat->ps;
	if (n->fields < 9) return;

	n->delayacct_blkio_ticks_diff = S_VALUE(o->delayacct_blkio_ticks, n->delayacct_blkio_ticks, itv);
	n->io.read_diff = S_VALUE(o->io.read_bytes, n->io.read_bytes, itv) / 1024;
	n->io.write_diff = S_VALUE(o->io.write_bytes, n->io.write_bytes, itv) / 1024;
	n->io.cancelled_write_diff = S_VALUE(o->io.cancelled_write_bytes, n->io.cancelled_write_bytes, itv) / 1024;

	n->utime_diff = SP_VALUE_100(o->utime, n->utime, itv);
	n->stime_diff = SP_VALUE_100(o->stime, n->stime, itv);
	n->gtime_diff = SP_VALUE_100(o->gtime, n->gtime, itv);
}

static void diff_pg_stats(pg_stat_list old_stats, pg_stat_list new_stats)
{
	unsigned long long itv;
	size_t old_pos = 0, new_pos = 0;

	if (old_stats.uptime == 0) return;
	itv = new_stats.uptime - old_stats.uptime;

	while (old_pos < old_stats.pos || new_pos < new_stats.pos) {
		if (new_pos >= new_stats.pos)
			old_stats.values[old_pos++].ps.free_cmdline = true;
		else if (old_pos >= old_stats.pos)
			read_proc_cmdline(new_stats.values + new_pos++);
		else if (old_stats.values[old_pos].pid == new_stats.values[new_pos].pid) {
			if (old_stats.values[old_pos].ps.start_time != new_stats.values[new_pos].ps.start_time)
				old_stats.values[old_pos].ps.free_cmdline = true;
			else if (old_stats.values[old_pos].type != PG_UNDEFINED && new_stats.values[new_pos].type == PG_UNDEFINED) {
				new_stats.values[new_pos].type = old_stats.values[old_pos].type;
				if (new_stats.values[new_pos].type == PG_LOGICAL_WORKER || new_stats.values[new_pos].type == PG_BG_WORKER)
					new_stats.values[new_pos].ps.cmdline = old_stats.values[old_pos].ps.cmdline;
			}

			read_proc_cmdline(new_stats.values + new_pos);
			calculate_stats_diff(old_stats.values + old_pos++, new_stats.values + new_pos++, itv);
		} else if (old_stats.values[old_pos].pid > new_stats.values[new_pos].pid)
			read_proc_cmdline(new_stats.values + new_pos++);
		else // old.pid < new.pid
			old_stats.values[old_pos++].ps.free_cmdline = true;
	}
}

static void get_pg_stat_activity(pg_stat_list *pg_stats)
{
	int ret;
	MemoryContext uppercxt = CurrentMemoryContext;

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());
	pgstat_report_activity(STATE_RUNNING, pg_stat_activity_query);

	/* We can now execute queries via SPI */
	ret = SPI_execute_plan(pg_stat_activity_query_plan, NULL, NULL, true, 0);

	if (ret != SPI_OK_SELECT)
		elog(FATAL, "cannot select from pg_stat_activity: error code %d", ret);

	pg_stats->recovery_in_progress = RecoveryInProgress();
	pg_stats->total_connections = SPI_processed + 1;
	pg_stats->active_connections = 0;

	if (SPI_processed > 0)
	{
		bool isnull, is_locker, is_idle;
		int a, len;
		Datum data;
		text *value;
		char *text_value;

		MemoryContext oldcxt = MemoryContextSwitchTo(uppercxt);
		for (a = 0; a < SPI_processed; a++) {
			pg_stat ps = {0, };
#if PG_VERSION_NUM >= 100000
			ps.type = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 8, &isnull));
#else
			ps.type = PG_BACKEND;
#endif

			is_idle = true;

			ps.pid = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 1, &isnull)); 
			is_locker = DatumGetBool(SPI_getbinval(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 7, &isnull));
			data = SPI_getbinval(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 6, &isnull);

			if (!isnull) {
				value = PG_DETOAST_DATUM_PACKED(data);
				text_value = VARDATA_ANY(value);
				len = VARSIZE_ANY_EXHDR(value);
				is_idle = len == 4 && !strncmp(text_value, "idle", 4);
#if PG_VERSION_NUM < 100000
				if (strncmp(text_value, "autovacuum: ", 12) == 0)
					ps.type = PG_AUTOVAC_WORKER;
#endif
			}

			if (is_locker || !is_idle || ps.type != PG_BACKEND) {
				ps.query = isnull || len == 0 ? NULL : json_escape_string_len(text_value, len);

				data = SPI_getbinval(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 2, &isnull);
				if (isnull) ps.datname = NULL;
				else {
					value = PG_DETOAST_DATUM_PACKED(data);
					ps.datname = json_escape_string_len(VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value));
				}

				data = SPI_getbinval(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 3, &isnull);
				if (isnull) ps.usename = NULL;
				else {
					value = PG_DETOAST_DATUM_PACKED(data);
					ps.usename = json_escape_string_len(VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value));
				}

				ps.age = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 4, &isnull));
				if (isnull) ps.age = -1;
				ps.locked_by = SPI_getvalue(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 5);
				pg_stats->active_connections++;
			}

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
	pg_stats->uptime = system_stats_old.uptime;

	if (pg_stats->pos > 0)
		qsort(pg_stats->values, pg_stats->pos, sizeof(pg_stat), pg_stat_cmp);
}

pg_stat_list get_postgres_stats(void)
{
	pg_stat_list pg_stats_tmp;

	read_procfs(&proc_stats);

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
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	pg_stat_activity_query_plan = SPI_prepare(pg_stat_activity_query, 0, NULL);
	if (pg_stat_activity_query_plan == NULL)
		elog(FATAL, "pg_stat_activity_query: SPI_prepare returned %d", SPI_result);

	if (SPI_keepplan(pg_stat_activity_query_plan))
		elog(FATAL, "pg_stat_activity_query: SPI_keepplan failed");

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();

	mem_page_size = getpagesize() / 1024;
	pg_stat_list_init(&pg_stats_current);
	pg_stat_list_init(&pg_stats_new);

	proc_stats.values = palloc(sizeof(proc_stat) * (proc_stats.size = pg_stats_current.size));

	cmdline_prefix_len = 10;

#if PG_VERSION_NUM >= 90500
	if (*cluster_name != '\0')
		cmdline_prefix_len += strlen(cluster_name) + 2;
#endif

	cmdline_prefix = palloc(cmdline_prefix_len + 1);
	strcpy(cmdline_prefix, "postgres: ");
#if PG_VERSION_NUM >= 90500
	if (*cluster_name != '\0') {
		strcpy(cmdline_prefix + 10, cluster_name);
		strcpy(cmdline_prefix + cmdline_prefix_len - 2, ": ");
	}
#endif

	sprintf(postmaster_pid, " %d ", PostmasterPid);
	postmaster_pid_len = strlen(postmaster_pid);
}
