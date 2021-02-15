#include <unistd.h>
#include <dirent.h>

#include "postgres.h"

#include "miscadmin.h"

#include "access/htup_details.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "executor/spi.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "pgstat.h"
#include "replication/walreceiver.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#if PG_VERSION_NUM < 90400
#include "utils/tqual.h"
#endif
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/predicate_internals.h"
#include "storage/procarray.h"

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
static pg_stat pg_stats_current;
static pg_stat pg_stats_new;

typedef struct {
	pg_stat_activity	*p;		/* pointer to a corresponding pg_stat_activity entry */
	uint32				pid;
#if PG_VERSION_NUM < 90600
	uint32				field1; /* a 32-bit ID field */
	uint32				field2; /* a 32-bit ID field */
	uint32				field3; /* a 32-bit ID field */
	uint16				field4; /* a 16-bit ID field */
	uint8				type;	/* see enum LockTagType */
	bool				granted;
#endif
} _lock;

#if PG_VERSION_NUM >= 90600
#define UINT32_ACCESS_ONCE(var)		((uint32)(*((volatile uint32 *)&(var))))
#define IS_GRANTED(l) false
#else
#define IS_GRANTED(l) l.granted
static int cmp_lock(const void *arg1, const void *arg2)
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
#endif

static _lock *get_pg_locks(int *num_locks)
{
	_lock				*locks = NULL;
	LockData			*lockData = GetLockStatusData();
#if PG_VERSION_NUM < 90600
	PredicateLockData	*predLockData = GetPredicateLockStatusData();

	*num_locks = 2 * lockData->nelements + predLockData->nelements;
#else
	*num_locks = lockData->nelements;
#endif

	if (*num_locks > 0)
	{
		int		i, j = 0;

		locks = palloc0(*num_locks * sizeof(_lock));

		for (i = 0; i < lockData->nelements;)
		{
			_lock			 *l;
			LockInstanceData *instance = &(lockData->locks[i]);
			bool			  granted = false;

			if (instance->holdMask)
			{
				instance->holdMask = 0;
				granted = true;
			}

			if (!granted)
			{
				i++;
				if (instance->waitLockMode == NoLock)
					continue;
			}
#if PG_VERSION_NUM >= 90600
			else continue;
#endif
			l = locks + j++;
			l->pid = instance->pid;
#if PG_VERSION_NUM < 90600
			l->granted = granted;
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
#endif
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
		if ((*num_locks = j) > 1)
			qsort(locks, *num_locks, sizeof(_lock), cmp_lock);
#endif
	}
	return locks;
}

static pg_stat_activity *find_pg_proc_by_pid(pg_stat_activity_list pg_stats, uint32 pid)
{
	pg_stat_activity *low = pg_stats.values;
	pg_stat_activity *high = low + pg_stats.pos - 1;

	while (low <= high)
	{
		pg_stat_activity *middle = low + (high - low) / 2;

		if (pid < middle->pid)
			high = middle - 1;
		else if (pid > middle->pid)
			low = middle + 1;
		else
			return middle;
	}

	return NULL;
}

static void update_blockers(pg_stat_activity *blocked, pg_stat_activity *blocker)
{
	blocker->is_blocker = true;
	if (blocked->blockers)
		((uint32 *)blocked->blockers)[blocked->num_blockers] = blocker->pid;
	else
		blocked->blockers = blocker->pid;
	blocked->num_blockers++;
}

static void fix_blockers_array(pg_stat_activity *blocked)
{
	if (blocked->num_blockers < 2)
	{
		uint32 tmp = blocked->num_blockers == 1 ? *((uint32 *)blocked->blockers) : 0;
		pfree((uint32 *)blocked->blockers);
		blocked->blockers = tmp;
	}
}

static void enrich_pg_stats(MemoryContext othercxt, pg_stat_activity_list pg_stats, _lock *locks, int num_locks)
{
	int	i, j, k, num_blockers;

	for (i = 0; i < num_locks; i++)
	{
		_lock		*l = &locks[i];
#if PG_VERSION_NUM < 90600
		int			granted = -1;
#endif

		if (IS_GRANTED((*l)) || !(l->p = find_pg_proc_by_pid(pg_stats, l->pid)))
			continue;

#if PG_VERSION_NUM < 90600
		/* find block of processes waiting for the same resource */
		for (j = i + 1, num_blockers = 0; j < num_locks; j++)
		{
			_lock *m = &locks[j];

			if (l->type != m->type || l->field1 != m->field1 || l->field2 != m->field2
					|| l->field3 != m->field3 || l->field4 != m->field4) break;

			m->p = find_pg_proc_by_pid(pg_stats, m->pid);
			if (m->p && IS_GRANTED((*m)))
			{
				if (granted == -1) granted = j;
				num_blockers++;
			}
		}

		if (granted == -1)
			goto adjust_i;

		for (k = i; k < granted; k++)
		{
			l = &locks[k];
			if (l->p && !l->p->blockers) /* we assume that process can't wait for more than one lock */
			{
				int m;

				if (num_blockers > 1)
					l->p->blockers = (uintptr_t) palloc(num_blockers * sizeof(uint32));

				for (m = granted; m < j; m++)
					if (locks[m].p && l->pid != locks[m].pid)
						update_blockers(l->p, locks[m].p);

				if (num_blockers > 1)
					fix_blockers_array(l->p);
			}
		}

adjust_i:
		i = j - 1;
#else
		{
			Datum		 *blockers;
			ArrayType	 *elems;
			MemoryContext oldcxt = MemoryContextSwitchTo(othercxt);
			elems = DatumGetArrayTypeP(DirectFunctionCall1(pg_blocking_pids, Int32GetDatum(l->pid)));
			deconstruct_array(elems, INT4OID, sizeof(int32), true, 'i', &blockers, NULL, &num_blockers);
			MemoryContextSwitchTo(oldcxt);

			if (num_blockers > 1)
				l->p->blockers = (uintptr_t) palloc(num_blockers * sizeof(uint32));

			for (j = 0; j < num_blockers; ++j)
			{
				bool e = false;
				int m;
				pg_stat_activity *blocker;

				if ((k = DatumGetInt32(blockers[j])) == 0) continue;

				for (m = 0; m < l->p->num_blockers; m++)
					if ((e = (((uint32 *)l->p->blockers)[m] == k)))
						break;

				if (!e && (blocker = find_pg_proc_by_pid(pg_stats, k)))
					update_blockers(l->p, blocker);
			}

			if (num_blockers > 1)
				fix_blockers_array(l->p);
		}
#endif
	}
}


static void pg_stat_activity_list_init(pg_stat_activity_list *list)
{
	list->pos = 0;
	list->size = MaxBackends + 17;
	list->values = palloc(sizeof(pg_stat_activity) * list->size);
}

static void pg_stat_activity_list_free_resources(pg_stat_activity_list *list)
{
	size_t i;
	for (i = 0; i < list->pos; ++i) {
		pg_stat_activity ps = list->values[i];
		FREE(ps.query);
		FREE(ps.usename);
		FREE(ps.datname);
		if (ps.num_blockers > 1)
			pfree((uint32 *)ps.blockers);

		if (ps.ps.free_cmdline)
			FREE(ps.ps.cmdline);
	}
	list->pos = 0;
}

static void pg_stat_activity_list_add(pg_stat_activity_list *list, pg_stat_activity ps)
{
	if (list->values == NULL)
		list->pos = list->size = 0;

	if (list->pos >= list->size) {
		int new_size = list->size > 0 ? list->size * 2 : 7;
		list->size = new_size;
		list->values = repalloc(list->values, sizeof(pg_stat_activity)*new_size);
	}
	list->values[list->pos++] = ps;
}

static void db_stat_list_init(db_stat_list *list)
{
	list->pos = 0;
	list->size = 3;
	list->values = palloc(sizeof(db_stat) * list->size);
}

static void db_stat_list_free_resources(db_stat_list *list)
{
	size_t i;
	for (i = 0; i < list->pos; ++i)
		FREE(list->values[i].datname);
	list->pos = 0;
}

static void db_stat_list_add(db_stat_list *list, db_stat ds)
{
	if (list->values == NULL)
		list->pos = list->size = 0;

	if (list->pos >= list->size) {
		int new_size = list->size > 0 ? list->size * 2 : 7;
		list->size = new_size;
		list->values = repalloc(list->values, sizeof(db_stat)*new_size);
	}
	list->values[list->pos++] = ds;
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
	ssize_t n;
	char *p, *endptr, buf[255];
	unsigned long resident = 0, share = 0;
	int fd = open(proc_file, O_RDONLY);
	if (fd < 0) return 0;
	if ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = '\0';
		for (p = buf; *p != ' ' && *p != '\0'; ++p);
		if (*p == ' ') {
			resident = strtoul(p + 1, &endptr, 10);
			if (endptr > p + 1 && *endptr == ' ')
				share = strtoul(endptr + 1, NULL, 10);
		}
	}
	close(fd);
	return (unsigned long long)(resident - share) * mem_page_size;
}

#define TAB_ENTRY(STRING, TARGET) {STRING, sizeof(STRING) - 1, TARGET}
#define IO_TAB(NAME) TAB_ENTRY(#NAME ": ", &pi.NAME)

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
		IO_TAB(read_bytes),
		IO_TAB(write_bytes),
		IO_TAB(cancelled_write_bytes),
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
	ssize_t n;
	char *t, buf[512];
	proc_stat ps = {0,};
	int statfd = open(proc_file, O_RDONLY);

	if (statfd < 0)
		return ps;

	if ((n = read(statfd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = '\0';
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

	close(statfd);
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

static int pg_stat_activity_cmp(const void *el1, const void *el2)
{
	return ((const pg_stat_activity *) el1)->pid - ((const pg_stat_activity *) el2)->pid;
}

static int db_stat_cmp(const void *el1, const void *el2)
{
	return ((const db_stat *) el1)->databaseid - ((const db_stat *) el2)->databaseid;
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
		elog(WARNING, "couldn't open '/proc'");
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
		elog(WARNING, "error reading /proc");

	closedir(proc);

	if (list->pos > 0)
		qsort(list->values, list->pos, sizeof(proc_stat), proc_stat_cmp);
}

static void merge_stats(pg_stat_activity_list *pg_stats, proc_stat_list proc_stats)
{
	size_t pg_stats_pos = 0, proc_stats_pos = 0;
	size_t pg_stats_size = pg_stats->pos;

	/* Both list are sorted on pid so we need to traverse it only once */
	while (pg_stats_pos < pg_stats_size || proc_stats_pos < proc_stats.pos) {
		if (proc_stats_pos >= proc_stats.pos) { /* new backends? */
			break;
		} else if (pg_stats_pos >= pg_stats_size || // No more entries from pg_stats_activity (special process?)
				pg_stats->values[pg_stats_pos].pid > proc_stats.values[proc_stats_pos].pid) {
			pg_stat_activity pgs = {0,};
			pgs.ps = proc_stats.values[proc_stats_pos];
			pgs.pid = pgs.ps.pid;
			pgs.type = PG_UNDEFINED; /* unknown process */
			pg_stat_activity_list_add(pg_stats, pgs);
			++proc_stats_pos;
		} else if (pg_stats->values[pg_stats_pos].pid == proc_stats.values[proc_stats_pos].pid) {
			pg_stats->values[pg_stats_pos++].ps = proc_stats.values[proc_stats_pos++];
		} else { /* pg_stats->values[pg_pos].pid < proc_stats.values[ps_pos].pid, new backend? */
			++pg_stats_pos;
		}
	}

	if (pg_stats->pos > 0)
		qsort(pg_stats->values, pg_stats->pos, sizeof(pg_stat_activity), pg_stat_activity_cmp);
}

#define PROCESS_PATTERN " process"

#if PG_VERSION_NUM >= 130000
	#define SUFFIX_PATTERN ""
	#define UNKNOWN_PROC_NAME UNKNOWN_NAME
#else
	#define SUFFIX_PATTERN "  "
	#define UNKNOWN_PROC_NAME "???" PROCESS_PATTERN SUFFIX_PATTERN
#endif

#define PARALLEL_WORKER_PROC_NAME PARALLEL_WORKER_NAME " for PID"
#define LOGICAL_LAUNCHER_PROC_NAME LOGICAL_LAUNCHER_NAME SUFFIX_PATTERN
#define LOGICAL_WORKER_PROC_NAME LOGICAL_WORKER_NAME " for"

#define BACKEND_ENTRY(CMDLINE_PATTERN, TYPE) TAB_ENTRY(CMDLINE_PATTERN " ", PG_##TYPE)

#define CMDLINE(TYPE) TYPE##_PROC_NAME
#define OTH_BACKEND(TYPE) BACKEND_ENTRY(CMDLINE(TYPE), TYPE)
#if PG_VERSION_NUM < 110000
	#define WAL_RECEIVER_PROC_NAME "wal receiver"
	#define WAL_SENDER_PROC_NAME "wal sender"
	#define WAL_WRITER_PROC_NAME "wal writer"
	#define BG_WRITER_PROC_NAME "writer"
	#define BG_WORKER_PROC_NAME "bgworker:"

	#define CMDLINE_PATTERN(TYPE) CMDLINE(TYPE) PROCESS_PATTERN
	#define BGWORKER(TYPE) BACKEND_ENTRY(CMDLINE(BG_WORKER) " " CMDLINE(TYPE), TYPE)
#else
	#define WAL_RECEIVER_PROC_NAME WAL_RECEIVER_NAME
	#define WAL_SENDER_PROC_NAME WAL_SENDER_NAME
	#define WAL_WRITER_PROC_NAME WAL_WRITER_NAME
	#define BG_WRITER_PROC_NAME "background writer"

	#define CMDLINE_PATTERN(TYPE) CMDLINE(TYPE)
	#define BGWORKER(TYPE) OTH_BACKEND(TYPE)
#endif
#define AUX_BACKEND(TYPE) BACKEND_ENTRY(CMDLINE_PATTERN(TYPE) SUFFIX_PATTERN, TYPE)

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
			AUX_BACKEND(ARCHIVER),
			AUX_BACKEND(STARTUP),
			AUX_BACKEND(WAL_RECEIVER),
			BACKEND_ENTRY(CMDLINE_PATTERN(WAL_SENDER), WAL_SENDER),
			AUX_BACKEND(AUTOVAC_LAUNCHER),
			AUX_BACKEND(AUTOVAC_WORKER),
#if PG_VERSION_NUM >= 90600
			BGWORKER(PARALLEL_WORKER),
#endif
#if PG_VERSION_NUM >= 100000
			BGWORKER(LOGICAL_LAUNCHER),
			BGWORKER(LOGICAL_WORKER),
#endif
#if PG_VERSION_NUM < 110000
			OTH_BACKEND(BG_WORKER),
#endif
			AUX_BACKEND(CHECKPOINTER),
			AUX_BACKEND(LOGGER),
			AUX_BACKEND(STATS_COLLECTOR),
			AUX_BACKEND(WAL_WRITER),
			AUX_BACKEND(BG_WRITER),
			OTH_BACKEND(UNKNOWN),
			{NULL, 0, PG_UNDEFINED}
		};

		for (j = 0; backend_tab[j].name != NULL; ++j)
			if (strncmp(cmd, backend_tab[j].name, backend_tab[j].name_len) == 0) {
				type = backend_tab[j].type;
				*rest = cmd + backend_tab[j].name_len;
				break;
			}

#if PG_VERSION_NUM >= 110000
		if (backend_tab[j].name == NULL) {
			size_t len = strlen(cmd) - sizeof(SUFFIX_PATTERN);
			if (len > 0 && strcmp(cmd + len, " " SUFFIX_PATTERN) == 0) {
				type = PG_BG_WORKER;
				*rest = cmd;
			}
		}
#endif
	}
	return type;
}

static void read_proc_cmdline(pg_stat_activity *stat)
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
			stat->ps.cmdline = json_escape_string_len(rest, strlen(rest) - sizeof(SUFFIX_PATTERN));
		else if (type == PG_PARALLEL_WORKER && *rest)
			stat->parent_pid = strtoul(rest, NULL, 10);
	}
	fclose(f);
}

static void calculate_stats_diff(pg_stat_activity *old_stat, pg_stat_activity *new_stat, unsigned long long itv)
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

static void diff_pg_stat_activity(pg_stat_activity_list old_activity, pg_stat_activity_list new_activity, unsigned long long itv)
{
	size_t old_pos = 0, new_pos = 0;


	while (old_pos < old_activity.pos || new_pos < new_activity.pos) {
		if (new_pos >= new_activity.pos)
			old_activity.values[old_pos++].ps.free_cmdline = true;
		else if (old_pos >= old_activity.pos)
			read_proc_cmdline(new_activity.values + new_pos++);
		else if (old_activity.values[old_pos].pid == new_activity.values[new_pos].pid) {
			if (old_activity.values[old_pos].ps.start_time != new_activity.values[new_pos].ps.start_time)
				old_activity.values[old_pos].ps.free_cmdline = true;
			else if (old_activity.values[old_pos].type != PG_UNDEFINED && new_activity.values[new_pos].type == PG_UNDEFINED) {
				new_activity.values[new_pos].type = old_activity.values[old_pos].type;
				if (new_activity.values[new_pos].type == PG_LOGICAL_WORKER || new_activity.values[new_pos].type == PG_BG_WORKER)
					new_activity.values[new_pos].ps.cmdline = old_activity.values[old_pos].ps.cmdline;
			}

			read_proc_cmdline(new_activity.values + new_pos);
			calculate_stats_diff(old_activity.values + old_pos++, new_activity.values + new_pos++, itv);
		} else if (old_activity.values[old_pos].pid > new_activity.values[new_pos].pid)
			read_proc_cmdline(new_activity.values + new_pos++);
		else // old.pid < new.pid
			old_activity.values[old_pos++].ps.free_cmdline = true;
	}


}

static void diff_db_stats(db_stat_list old_db, db_stat_list new_db, unsigned long long itv)
{
	size_t old_pos = 0, new_pos = 0;

	while (old_pos < old_db.pos && new_pos < new_db.pos) {
		if (old_db.values[old_pos].databaseid == new_db.values[new_pos].databaseid) {
			new_db.values[new_pos].n_xact_commit_diff = S_VALUE(old_db.values[old_pos].n_xact_commit, new_db.values[new_pos].n_xact_commit, itv);
			new_db.values[new_pos].n_xact_rollback_diff = S_VALUE(old_db.values[old_pos].n_xact_rollback, new_db.values[new_pos].n_xact_rollback, itv);
			new_db.values[new_pos].n_blocks_fetched_diff = S_VALUE(old_db.values[old_pos].n_blocks_fetched, new_db.values[new_pos].n_blocks_fetched, itv);
			new_db.values[new_pos].n_blocks_hit_diff = S_VALUE(old_db.values[old_pos].n_blocks_hit, new_db.values[new_pos].n_blocks_hit, itv);
			new_db.values[new_pos].n_tuples_returned_diff = S_VALUE(old_db.values[old_pos].n_tuples_returned, new_db.values[new_pos].n_tuples_returned, itv);
			new_db.values[new_pos].n_tuples_fetched_diff = S_VALUE(old_db.values[old_pos].n_tuples_fetched, new_db.values[new_pos].n_tuples_fetched, itv);
			new_db.values[new_pos].n_tuples_updated_diff = S_VALUE(old_db.values[old_pos].n_tuples_updated, new_db.values[new_pos].n_tuples_updated, itv);
			new_db.values[new_pos].n_tuples_inserted_diff = S_VALUE(old_db.values[old_pos].n_tuples_inserted, new_db.values[new_pos].n_tuples_inserted, itv);
			new_db.values[new_pos].n_tuples_deleted_diff = S_VALUE(old_db.values[old_pos].n_tuples_deleted, new_db.values[new_pos].n_tuples_deleted, itv);
			new_db.values[new_pos].n_temp_files_diff = S_VALUE(old_db.values[old_pos].n_temp_files, new_db.values[new_pos].n_temp_files, itv);
			new_db.values[new_pos].n_temp_bytes_diff = S_VALUE(old_db.values[old_pos].n_temp_bytes, new_db.values[new_pos].n_temp_bytes, itv);
			if (new_db.track_io_timing) {
				new_db.values[new_pos].n_block_read_time_diff = MINIMUM(S_VALUE(old_db.values[old_pos].n_block_read_time, new_db.values[new_pos].n_block_read_time, itv)/10000.0, 100.0);
				new_db.values[new_pos].n_block_write_time_diff = MINIMUM(S_VALUE(old_db.values[old_pos].n_block_write_time, new_db.values[new_pos].n_block_write_time, itv)/10000.0, 100.0);
			}
			old_pos++;
			new_pos++;
		} else if (old_db.values[old_pos].databaseid > new_db.values[new_pos].databaseid)
			new_pos++;
		else /* old_db.values[old_pos].databaseid < new_db.values[new_pos].databaseid */
			old_pos++;
	}
}

static void diff_pg_stats(pg_stat old_stats, pg_stat new_stats)
{
	unsigned long long itv;

	wal_metrics o = old_stats.wal_metrics;
	wal_metrics n = new_stats.wal_metrics;

	if (old_stats.uptime == 0) return;
	itv = new_stats.uptime - old_stats.uptime;

	diff_pg_stat_activity(old_stats.activity, new_stats.activity, itv);
	diff_db_stats(old_stats.db, new_stats.db, itv);

	n.current_diff = S_VALUE(o.current_wal_lsn, n.current_wal_lsn, itv) / 1024;
	n.receive_diff = S_VALUE(o.last_wal_receive_lsn, o.last_wal_receive_lsn, itv) / 1024;
	n.replay_diff  = S_VALUE(o.last_wal_replay_lsn, o.last_wal_replay_lsn, itv) / 1024;
}

static double calculate_age(TimestampTz ts)
{
	double ret = (GetCurrentTimestamp() - ts) / 1000000.0;
	return ret > 0 ? ret : 0;
}

#if PG_VERSION_NUM >= 100000
static PgBackendType map_backend_type(BackendType type)
{
	switch (type)
	{
		case B_AUTOVAC_LAUNCHER:
			return PG_AUTOVAC_LAUNCHER;
		case B_AUTOVAC_WORKER:
			return PG_AUTOVAC_WORKER;
		case B_BACKEND:
			return PG_BACKEND;
/*		case B_BG_WORKER:
			return PG_BG_WORKER;*/
		case B_BG_WRITER:
			return PG_BG_WRITER;
		case B_CHECKPOINTER:
			return PG_CHECKPOINTER;
		case B_STARTUP:
			return PG_STARTUP;
		case B_WAL_RECEIVER:
			return PG_WAL_RECEIVER;
		case B_WAL_SENDER:
			return PG_WAL_SENDER;
		case B_WAL_WRITER:
			return PG_WAL_WRITER;
#if PG_VERSION_NUM >= 130000
		case B_INVALID:
			return PG_UNKNOWN;
		case B_ARCHIVER:
			return PG_ARCHIVER;
		case B_STATS_COLLECTOR:
			return PG_STATS_COLLECTOR;
		case B_LOGGER:
			return PG_LOGGER;
#endif
		case B_BG_WORKER:
		default:
			return PG_UNDEFINED;
	}
}
#endif

static void get_pg_stat_activity(pg_stat_activity_list *pg_stats)
{
	bool		 init_postgres = false;
	int			 i, num_backends, num_locks;
	_lock		*locks;
	MemoryContext othercxt, oldcxt;

	num_backends = pgstat_fetch_stat_numbackends();

#if PG_VERSION_NUM >= 90600
	othercxt = AllocSetContextCreate(CurrentMemoryContext, "Locks snapshot", ALLOCSET_SMALL_SIZES);
#else
	othercxt = AllocSetContextCreate(CurrentMemoryContext, "Locks snapshot", ALLOCSET_SMALL_MINSIZE,
									ALLOCSET_SMALL_INITSIZE, ALLOCSET_SMALL_MAXSIZE);
#endif
	oldcxt = MemoryContextSwitchTo(othercxt);
	locks = get_pg_locks(&num_locks);
	MemoryContextSwitchTo(oldcxt);

	pg_stats->total_connections = 1;  /* bg_mon */
	pg_stats->active_connections = 0;
	pg_stats->idle_in_transaction_connections = 0;

	for (i = 1; i <= num_backends; ++i)
	{
		PgBackendStatus *beentry = pgstat_fetch_stat_beentry(i);
		if (beentry && beentry->st_procpid != MyProcPid)
		{
			pg_stat_activity ps = {beentry->st_procpid, 0,};
#if PG_VERSION_NUM >= 90600
			PGPROC *proc = BackendPidGetProc(beentry->st_procpid);
#if PG_VERSION_NUM >= 100000
			if (proc == NULL && (beentry->st_backendType != B_BACKEND))
				proc = AuxiliaryPidGetProc(beentry->st_procpid);
#endif
			if (proc != NULL)
				ps.raw_wait_event = UINT32_ACCESS_ONCE(proc->wait_event_info);

			if (proc != NULL && proc->lockGroupLeader && proc->lockGroupLeader->pid != beentry->st_procpid) {
				ps.parent_pid = proc->lockGroupLeader->pid;
				ps.type = PG_PARALLEL_WORKER;
			}
			else
#endif
#if PG_VERSION_NUM >= 100000
				ps.type = map_backend_type(beentry->st_backendType);
#else
			{
				SockAddr zero_clientaddr = {0,};
				/* hacky way to distinguish between normal backend and presumably bgworker */
				if (memcmp(&(beentry->st_clientaddr), &zero_clientaddr, sizeof(zero_clientaddr)) == 0
						|| beentry->st_databaseid == 0)
					ps.type = PG_UNDEFINED;
				else if (beentry->st_activity && 0 == strncmp(beentry->st_activity, "autovacuum: ", 12))
					ps.type = PG_AUTOVAC_WORKER;
				else ps.type = PG_BACKEND;
			}
#endif
			if ((ps.databaseid = beentry->st_databaseid))
				pg_stats->total_connections++;
			ps.userid = beentry->st_userid;
			ps.state = beentry->st_state;

			/* it is proven experimentally that it is safe to run InitPostgres only when we
			 * got some other backends connected which already initialized system caches.
			 * In such case there will be entries with valid databaseid and userid. */
			if (ps.userid && ps.databaseid && IsInitProcessingMode())
				init_postgres = !IsBinaryUpgrade; /* avoid opening a connection during pg_upgrade */

			if (ps.state == STATE_IDLEINTRANSACTION)
			{
				if (ps.databaseid)
					pg_stats->idle_in_transaction_connections++;
				if (beentry->st_xact_start_timestamp == beentry->st_state_start_timestamp)
					ps.idle_in_transaction_age = 0;
				else
					ps.idle_in_transaction_age = calculate_age(beentry->st_state_start_timestamp);
			} else if (ps.state == STATE_RUNNING) {
				if (ps.databaseid)
					pg_stats->active_connections++;
#if PG_VERSION_NUM >= 110000
				if (*beentry->st_activity_raw)
				{
					char	*query;
					oldcxt = MemoryContextSwitchTo(othercxt);
					query = pgstat_clip_activity(beentry->st_activity_raw);
					MemoryContextSwitchTo(oldcxt);
					ps.query = json_escape_string(query);
				}
#else
				ps.query = *beentry->st_activity ? json_escape_string(beentry->st_activity) : NULL;
#endif
			}

			if (beentry->st_xact_start_timestamp)
				ps.age = calculate_age(beentry->st_xact_start_timestamp);
			else if (ps.state == STATE_RUNNING && beentry->st_activity_start_timestamp)
				ps.age = calculate_age(beentry->st_activity_start_timestamp);
			else ps.age = -1;

			pg_stat_activity_list_add(pg_stats, ps);
		}
	}

	pgstat_clear_snapshot();

	if (pg_stats->pos > 1)
		qsort(pg_stats->values, pg_stats->pos, sizeof(pg_stat_activity), pg_stat_activity_cmp);

	if (num_locks > 0)
		enrich_pg_stats(othercxt, *pg_stats, locks, num_locks);

	MemoryContextDelete(othercxt);

	if (init_postgres)
	{
#if PG_VERSION_NUM >= 110000
		InitPostgres("postgres", InvalidOid, NULL, InvalidOid, NULL, false);
#elif PG_VERSION_NUM >= 90500
		InitPostgres("postgres", InvalidOid, NULL, InvalidOid, NULL);
#else
		InitPostgres("postgres", InvalidOid, NULL, NULL);
#endif
		SetProcessingMode(NormalProcessing);
		MemoryContextSwitchTo(oldcxt);
	}
}

static void resolve_database_and_user_ids(pg_stat_activity_list activity, MemoryContext resultcxt)
{
	MemoryContext oldcxt;
	int i;

	for (i = 0; i < activity.pos; ++i)
	{
		pg_stat_activity *ps = activity.values + i;
		if (ps->is_blocker || ps->state != STATE_IDLE || ps->type != PG_BACKEND) {
			if (ps->userid)
			{
				HeapTuple roleTup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(ps->userid));
				if (HeapTupleIsValid(roleTup))
				{
					oldcxt = MemoryContextSwitchTo(resultcxt);
					ps->usename = json_escape_string(NameStr(((Form_pg_authid) GETSTRUCT(roleTup))->rolname));
					MemoryContextSwitchTo(oldcxt);
					ReleaseSysCache(roleTup);
				}
			}

			if (ps->databaseid)
			{
				HeapTuple databaseTup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(ps->databaseid));
				if (HeapTupleIsValid(databaseTup))
				{
					oldcxt = MemoryContextSwitchTo(resultcxt);
					ps->datname = json_escape_string(NameStr(((Form_pg_database) GETSTRUCT(databaseTup))->datname));
					MemoryContextSwitchTo(oldcxt);
					ReleaseSysCache(databaseTup);
				}
			}
		}
	}
}

static void get_database_stats(db_stat_list *db, MemoryContext resultcxt)
{
#if PG_VERSION_NUM < 120000
	Relation rel = heap_open(DatabaseRelationId, AccessShareLock);
#else
	Relation rel = table_open(DatabaseRelationId, AccessShareLock);
#endif
#if PG_VERSION_NUM < 90400
	HeapScanDesc scan = heap_beginscan(rel, SnapshotNow, 0, NULL);
#elif PG_VERSION_NUM < 120000
	HeapScanDesc scan = heap_beginscan_catalog(rel, 0, NULL);
#else
	TableScanDesc scan = table_beginscan_catalog(rel, 0, NULL);
#endif
	HeapTuple tup;

	while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))
	{
		Form_pg_database pgdatabase = (Form_pg_database) GETSTRUCT(tup);
#if PG_VERSION_NUM < 120000
		Oid oid = HeapTupleGetOid(tup);
#else
		Oid oid = pgdatabase->oid;
#endif
		PgStat_StatDBEntry *dbentry = pgstat_fetch_stat_dbentry(oid);

		if (dbentry != NULL)
		{
			MemoryContext oldcxt = MemoryContextSwitchTo(resultcxt);

			db_stat ds = {0,};
			ds.databaseid = oid;
			ds.datname = json_escape_string(NameStr(pgdatabase->datname));

			ds.n_xact_commit = (int64) (dbentry->n_xact_commit);
			ds.n_xact_rollback = (int64) (dbentry->n_xact_rollback);
			ds.n_blocks_fetched = (int64) (dbentry->n_blocks_fetched);
			ds.n_blocks_hit = (int64) (dbentry->n_blocks_hit);
			ds.n_tuples_returned = (int64) (dbentry->n_tuples_returned);
			ds.n_tuples_fetched = (int64) (dbentry->n_tuples_fetched);
			ds.n_tuples_inserted = (int64) (dbentry->n_tuples_inserted);
			ds.n_tuples_updated = (int64) (dbentry->n_tuples_updated);
			ds.n_tuples_deleted = (int64) (dbentry->n_tuples_deleted);
			ds.n_conflict_tablespace = (int64) (dbentry->n_conflict_tablespace);
			ds.n_conflict_lock = (int64) (dbentry->n_conflict_lock);
			ds.n_conflict_snapshot = (int64) (dbentry->n_conflict_snapshot);
			ds.n_conflict_bufferpin = (int64) (dbentry->n_conflict_bufferpin);
			ds.n_conflict_startup_deadlock = (int64) (dbentry->n_conflict_startup_deadlock);
			ds.n_temp_files = (int64) (dbentry->n_temp_files);
			ds.n_temp_bytes = (int64) (dbentry->n_temp_bytes);
			ds.n_deadlocks = (int64) (dbentry->n_deadlocks);
#if PG_VERSION_NUM >= 120000
			ds.n_checksum_failures = (int64) (dbentry->n_checksum_failures);
			ds.last_checksum_failure = (int64) (dbentry->last_checksum_failure);
#endif
			ds.n_block_read_time = (int64) (dbentry->n_block_read_time); /* times in microseconds */
			ds.n_block_write_time = (int64) (dbentry->n_block_write_time);

			db_stat_list_add(db, ds);

			MemoryContextSwitchTo(oldcxt);
		}
	}

#if PG_VERSION_NUM < 120000
	heap_endscan(scan);
	heap_close(rel, AccessShareLock);
#else
	table_endscan(scan);
	table_close(rel, AccessShareLock);
#endif
}

pg_stat get_postgres_stats(void)
{
	pg_stat pg_stats_tmp;

	read_procfs(&proc_stats);

	pg_stat_activity_list_free_resources(&pg_stats_new.activity);
	db_stat_list_free_resources(&pg_stats_new.db);

	get_pg_stat_activity(&pg_stats_new.activity);

	if (IsNormalProcessingMode()) {
		MemoryContext resultcxt = CurrentMemoryContext;

		StartTransactionCommand();
		(void) GetTransactionSnapshot();

		resolve_database_and_user_ids(pg_stats_new.activity, resultcxt);
		get_database_stats(&pg_stats_new.db, resultcxt);
		pg_stats_new.db.track_io_timing = track_io_timing;

		CommitTransactionCommand();
		MemoryContextSwitchTo(resultcxt);
	}

	pg_stats_new.uptime = system_stats_old.uptime;

	pg_stats_new.recovery_in_progress = RecoveryInProgress();
	pg_stats_new.wal_metrics.last_wal_receive_lsn = GetWalRcvWriteRecPtr(NULL, NULL);
	pg_stats_new.wal_metrics.is_wal_replay_paused = RecoveryIsPaused();
	pg_stats_new.wal_metrics.last_xact_replay_timestamp = GetLatestXTime();

	pg_stats_new.wal_metrics.last_wal_replay_lsn = GetXLogReplayRecPtr(NULL);
	pg_stats_new.wal_metrics.current_wal_lsn = GetXLogWriteRecPtr();
	#if PG_VERSION_NUM >= 130000 
		pg_stats_new.wal_metrics.last_wal_receive_lsn = GetWalRcvFlushRecPtr(NULL, NULL);
	#else
		pg_stats_new.wal_metrics.last_wal_receive_lsn = GetWalRcvWriteRecPtr(NULL, NULL);
	#endif

	merge_stats(&pg_stats_new.activity, proc_stats);

	if (pg_stats_new.db.pos > 1)
		qsort(pg_stats_new.db.values, pg_stats_new.db.pos, sizeof(db_stat), db_stat_cmp);

	diff_pg_stats(pg_stats_current, pg_stats_new);

	pg_stats_tmp = pg_stats_current;
	pg_stats_current = pg_stats_new;
	pg_stats_new = pg_stats_tmp;

	return pg_stats_current;
}

void postgres_stats_init(void)
{
	mem_page_size = getpagesize() / 1024;
	pg_stat_activity_list_init(&pg_stats_current.activity);
	pg_stat_activity_list_init(&pg_stats_new.activity);
	db_stat_list_init(&pg_stats_current.db);
	db_stat_list_init(&pg_stats_new.db);

	proc_stats.values = palloc(sizeof(proc_stat) * (proc_stats.size = pg_stats_current.activity.size));

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
