#include <unistd.h>
#include <dirent.h>

#include "postgres.h"

#include "miscadmin.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "executor/spi.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "storage/ipc.h"
#include "storage/predicate_internals.h"

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

typedef struct {
	proc_stat *values;
	size_t size;
	size_t pos;
} proc_stat_list;

static proc_stat_list proc_stats;
static pg_stat_list pg_stats_current;
static pg_stat_list pg_stats_new;

typedef struct {
	uint32		pid;
	uint32		field1; /* a 32-bit ID field */
	uint32		field2; /* a 32-bit ID field */
	uint32		field3; /* a 32-bit ID field */
	uint16		field4; /* a 16-bit ID field */
	uint8		type;	/* see enum LockTagType */
	bool		granted;
} _lock;

static int lock_cmp(const void *arg1, const void *arg2)
{
	const _lock *l1 = (const _lock *) arg1;
	const _lock *l2 = (const _lock *) arg2;

	if (l1->type > l2->type) return 1;
	if (l1->type < l2->type) return -1;

	if (l1->field1 > l2->field1) return 1;
	if (l1->field1 < l2->field1) return -1;

	if (l1->field2 > l2->field2) return 1;
	if (l1->field2 < l2->field2) return -1;

	if (l1->field3 > l2->field3) return 1;
	if (l1->field3 < l2->field3) return -1;

	if (l1->field4 > l2->field4) return 1;
	if (l1->field4 < l2->field4) return -1;

	if (l1->granted > l2->granted) return 1;
	if (l1->granted < l2->granted) return -1;

	if (l1->pid > l2->pid) return 1;
	if (l1->pid < l2->pid) return -1;

	return 0;
}

static _lock *get_pg_locks(int *num_locks)
{
	LockData		  *lockData;
	PredicateLockData *predLockData;
	_lock			  *locks = NULL;

	lockData = GetLockStatusData();
	predLockData = GetPredicateLockStatusData();

	if ((*num_locks = lockData->nelements + predLockData->nelements) > 0)
	{
		int		i, j = 0;

		locks = palloc0(*num_locks * sizeof(_lock));

		for (i = 0; i < lockData->nelements;)
		{
			_lock			 *l;
			LockInstanceData *instance = &(lockData->locks[i]);
			LOCKMODE		  mode = 0;
			bool			  granted = false;

			if (instance->holdMask)
				for (mode = 0; mode < MAX_LOCKMODES; mode++)
					if (instance->holdMask & LOCKBIT_ON(mode))
					{
						granted = true;
						instance->holdMask &= LOCKBIT_OFF(mode);
						break;
					}

			if (!granted)
			{
				i++;
				if (instance->waitLockMode != NoLock)
					mode = instance->waitLockMode;
				else
					continue;
			}
#if PG_VERSION_NUM >= 90600
			else continue;
#endif

			l = locks + j++;
			l->granted = granted;
			l->pid = instance->pid;
			l->type = instance->locktag.locktag_type;

			switch ((LockTagType) l->type)
			{
				default:
				case LOCKTAG_OBJECT:
				case LOCKTAG_USERLOCK:
				case LOCKTAG_TUPLE:
					l->field4 = instance->locktag.locktag_field4;
				case LOCKTAG_PAGE:
					l->field3 = instance->locktag.locktag_field3;
				case LOCKTAG_RELATION:
				case LOCKTAG_RELATION_EXTEND:
				case LOCKTAG_VIRTUALTRANSACTION:
					l->field2 = instance->locktag.locktag_field2;
				case LOCKTAG_TRANSACTION:
					l->field1 = instance->locktag.locktag_field1;
			}
		}

#if PG_VERSION_NUM < 90600
		for (i = 0; i < predLockData->nelements; ++i)
		{
			PREDICATELOCKTARGETTAG *predTag = &(predLockData->locktags[i]);
			PredicateLockTargetType	lockType = GET_PREDICATELOCKTARGETTAG_TYPE(*predTag);
			_lock				   *l = locks + j++;

			l->granted = true;
			l->pid = predLockData->xacts[i].pid;
			l->type = lockType == PREDLOCKTAG_RELATION ? lockType : lockType + 1;

			switch (lockType)
			{
				case PREDLOCKTAG_TUPLE:
					l->field4 = GET_PREDICATELOCKTARGETTAG_OFFSET(*predTag);
				case PREDLOCKTAG_PAGE:
					l->field3 = GET_PREDICATELOCKTARGETTAG_PAGE(*predTag);
				default:
					l->field2 = GET_PREDICATELOCKTARGETTAG_RELATION(*predTag);
					l->field1 = GET_PREDICATELOCKTARGETTAG_DB(*predTag);
			}
		}
#endif

		if ((*num_locks = j) > 1)
			qsort(locks, *num_locks, sizeof(_lock), lock_cmp);
	}

	return locks;
}

static pg_stat *find_pg_proc_by_pid(pg_stat_list pg_stats, uint32 pid)
{
	pg_stat *low = pg_stats.values;
	pg_stat *high = low + pg_stats.pos - 1;

	while (low <= high)
	{
		pg_stat *middle = low + (high - low) / 2;

		if (pid < middle->pid)
			high = middle - 1;
		else if (pid > middle->pid)
			low = middle + 1;
		else
			return middle;
	}

	return NULL;
}

static void update_blockers(pg_stat_list pg_stats, pg_stat *blocked, int blocker_pid)
{
	int			i;
	bool		e = false;
	pg_stat	   *blocker;

	for (i = 0; i < blocked->num_blockers; ++i)
		if ((e = (blocked->blocking_pids[i] == blocker_pid)))
			break;

	if (!e && (blocker = find_pg_proc_by_pid(pg_stats, blocker_pid)))
	{
		blocker->is_blocker = true;
		if (!blocked->blocking_pids)
			blocked->blocking_pids = palloc(pg_stats.pos * sizeof(uint32));
		blocked->blocking_pids[blocked->num_blockers++] = blocker_pid;
	}
}

static void enrich_pg_stats(pg_stat_list pg_stats, _lock *locks, int num_locks)
{
	int	i, j;

	for (i = 0; i < num_locks; i++)
	{
		pg_stat		*blocked;
		_lock		 l = locks[i];

		if (l.granted || !(blocked = find_pg_proc_by_pid(pg_stats, l.pid)))
			continue;

#if PG_VERSION_NUM < 90600
		for (j = i + 1; j < num_locks; j++)
		{
			_lock m = locks[j];

			if (l.type != m.type || l.field1 != m.field1 || l.field2 != m.field2
					|| l.field3 != m.field3 || l.field4 != m.field4) break;

			if (!m.granted) continue;

			if (l.pid != m.pid)
				update_blockers(pg_stats, blocked, m.pid);
		}
#else
		{
			int		   num_blockers;
			Datum	  *blockers;

			ArrayType *elems = DatumGetArrayTypeP(DirectFunctionCall1(pg_blocking_pids, Int32GetDatum(l.pid)));

			deconstruct_array(elems, INT4OID, sizeof(int32), true, 'i', &blockers, NULL, &num_blockers);
			for (j = 0; j < num_blockers; ++j)
				update_blockers(pg_stats, blocked, DatumGetInt32(blockers[j]));
		}
#endif
	}
}


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
		FREE(ps.blocking_pids);

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

	while (fgets(buf, sizeof(buf), iofd) && i < lengthof(io_tab) - 1) {
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
			++pg_stats_pos;
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
			if (type == PG_WAL_SENDER && stat->usename) {
				size_t len = strlen(stat->usename) - 2;
				if (strncmp(rest, stat->usename + 1, len) == 0 && rest[len] == ' ')
					rest += len + 1;
			}
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

static int cmp_int(const void *va, const void *vb)
{
	uint32 a = *((const uint32 *) va);
	uint32 b = *((const uint32 *) vb);

	if (a == b) return 0;
	return (a > b) ? 1 : -1;
}

static double calculate_age(TimestampTz ts)
{
	double ret = (GetCurrentTimestamp() - ts) / 1000000.0;
	return ret > 0 ? ret : 0;
}

static void get_pg_stat_activity(pg_stat_list *pg_stats)
{
	int			 i, num_backends, num_locks;
	_lock		*locks;
	MemoryContext othercxt, uppercxt = CurrentMemoryContext;

	num_backends = pgstat_fetch_stat_numbackends();

	othercxt = AllocSetContextCreate(uppercxt, "Locks snapshot", ALLOCSET_SMALL_MINSIZE,
									ALLOCSET_SMALL_INITSIZE, ALLOCSET_SMALL_MAXSIZE);
	MemoryContextSwitchTo(othercxt);
	locks = get_pg_locks(&num_locks);
	MemoryContextSwitchTo(uppercxt);

	for (i = 1; i <= num_backends; ++i)
	{
		PgBackendStatus *beentry = pgstat_fetch_stat_beentry(i);
		if (beentry
#if PG_VERSION_NUM >= 100000
				&& beentry->st_backendType != B_AUTOVAC_LAUNCHER
				&& beentry->st_backendType != B_BG_WRITER
				&& beentry->st_backendType != B_CHECKPOINTER
				&& beentry->st_backendType != B_STARTUP
				&& beentry->st_backendType != B_WAL_RECEIVER
				&& beentry->st_backendType != B_WAL_WRITER
#endif
				&& beentry->st_procpid != MyProcPid)
		{
			pg_stat ps = {0, };

			ps.pid = beentry->st_procpid;
			ps.databaseid = beentry->st_databaseid;
			ps.userid = beentry->st_userid;
			ps.state = beentry->st_state;

			/* it is proven experimetally that it is safe to run InitPostgres only when we
			 * got some other backends connected which already initialized system caches.
			 * In such case there will be entries with valid databaseid and userid. */
			if (ps.userid && ps.databaseid && IsInitProcessingMode())
			{
#if PG_VERSION_NUM >= 110000
				InitPostgres(NULL, InvalidOid, NULL, InvalidOid, NULL, false);
#elif PG_VERSION_NUM >= 90500
				InitPostgres(NULL, InvalidOid, NULL, InvalidOid, NULL);
#else
				InitPostgres(NULL, InvalidOid, NULL, NULL);
#endif
				SetProcessingMode(NormalProcessing);
			}

			if (ps.state == STATE_IDLEINTRANSACTION)
			{
				if (beentry->st_xact_start_timestamp == beentry->st_state_start_timestamp)
					ps.idle_in_transaction_age = 0;
				else
					ps.idle_in_transaction_age = calculate_age(beentry->st_state_start_timestamp);
			} else if (ps.state == STATE_RUNNING)
#if PG_VERSION_NUM >= 110000
				if (*beentry->st_activity_raw)
				{
					char	*query;
					MemoryContextSwitchTo(othercxt);
					query = pgstat_clip_activity(beentry->st_activity_raw);
					MemoryContextSwitchTo(uppercxt);
					ps.query = json_escape_string(query);
				}
#else
				ps.query = *beentry->st_activity ? json_escape_string(beentry->st_activity) : NULL;
#endif

			if (beentry->st_xact_start_timestamp)
				ps.age = calculate_age(beentry->st_xact_start_timestamp);
			else if (ps.state == STATE_RUNNING && beentry->st_activity_start_timestamp)
				ps.age = calculate_age(beentry->st_activity_start_timestamp);
			else ps.age = -1;

#if PG_VERSION_NUM >= 100000
			ps.type = beentry->st_backendType == B_BG_WORKER ? PG_UNDEFINED : beentry->st_backendType;
#else
			if (beentry->st_activity && 0 == strncmp(beentry->st_activity, "autovacuum: ", 12))
				ps.type = PG_AUTOVAC_WORKER;
			else ps.type = ps.databaseid == 0 ? ps.type = PG_UNDEFINED : PG_BACKEND;
#endif
			pg_stat_list_add(pg_stats, ps);
		}
	}

	pgstat_clear_snapshot();

	if ((num_backends = pg_stats->pos) > 1)
		qsort(pg_stats->values, pg_stats->pos, sizeof(pg_stat), pg_stat_cmp);

	if (num_locks > 0)
		enrich_pg_stats(*pg_stats, locks, num_locks);

	MemoryContextDelete(othercxt);

	pg_stats->recovery_in_progress = RecoveryInProgress();
	pg_stats->total_connections = num_backends;
	pg_stats->active_connections = 0;

	if (IsNormalProcessingMode())
	{
		StartTransactionCommand();
		(void) GetTransactionSnapshot();
		othercxt = MemoryContextSwitchTo(uppercxt);
	}

	for (i = 0; i < num_backends; ++i)
	{
		pg_stat *ps = pg_stats->values + i;
		if (ps->is_blocker || ps->state != STATE_IDLE || ps->type != PG_BACKEND) {
			if (ps->num_blockers > 1)
				qsort(ps->blocking_pids, ps->num_blockers, sizeof(uint32), cmp_int);

			if (ps->userid && IsNormalProcessingMode())
			{
				HeapTuple roleTup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(ps->userid));
				if (HeapTupleIsValid(roleTup))
				{
					ps->usename = json_escape_string(((Form_pg_authid) GETSTRUCT(roleTup))->rolname.data);
					ReleaseSysCache(roleTup);
				}
			}

			if (ps->databaseid && IsNormalProcessingMode())
			{
				HeapTuple databaseTup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(ps->databaseid));
				if (HeapTupleIsValid(databaseTup))
				{
					ps->datname = json_escape_string(((Form_pg_database) GETSTRUCT(databaseTup))->datname.data);
					ReleaseSysCache(databaseTup);
				}
			}

			pg_stats->active_connections++;
		}
	}

	if (IsNormalProcessingMode())
	{
		MemoryContextSwitchTo(othercxt);
		CommitTransactionCommand();
	}
}

pg_stat_list get_postgres_stats(void)
{
	pg_stat_list pg_stats_tmp;

	read_procfs(&proc_stats);

	pg_stat_list_free_resources(&pg_stats_new);

	get_pg_stat_activity(&pg_stats_new);

	pg_stats_new.uptime = system_stats_old.uptime;

	merge_stats(&pg_stats_new, proc_stats);
	diff_pg_stats(pg_stats_current, pg_stats_new);

	pg_stats_tmp = pg_stats_current;
	pg_stats_current = pg_stats_new;
	pg_stats_new = pg_stats_tmp;

	return pg_stats_current;
}

void postgres_stats_init(void)
{
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
