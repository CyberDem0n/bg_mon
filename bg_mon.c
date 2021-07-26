#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/thread.h>

#include "postgres.h"

/* These are always necessary for a bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"

/* these headers are used by this particular worker's code */
#include "pgstat.h"
#include "tcop/utility.h"
#include "utils/datetime.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "safety_funcs.h"
#include "net_stats.h"
#include "postgres_stats.h"
#include "disk_stats.h"
#include "system_stats.h"
#ifdef HAS_LIBBROTLI
#include "brotli_utils.h"
#endif

PG_MODULE_MAGIC;

void _PG_init(void);
void bg_mon_main(Datum);

extern TimestampTz PgStartTime;
pg_time_t pg_start_time;
extern int MaxConnections;
extern char *DataDir;

static pthread_mutex_t lock;

/* flags set by signal handlers */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* GUC variables */
static int bg_mon_naptime_guc = 1;
static char *bg_mon_listen_address_guc;
static char *bg_mon_listen_address = NULL;
static int bg_mon_port_guc = 8080;
static int bg_mon_port = 8080;

static struct evbuffer *evbuffer = NULL;
#ifdef HAS_LIBBROTLI
static int bg_mon_num_buckets = 20;
static pthread_mutex_t agg_lock;
struct aggregated_stats {
	time_t minute;
	struct compression_state state;
};
static struct aggregated_stats *aggregated;
static struct aggregated_stats *prev = NULL;

static bool accepts_brotli(struct evhttp_request *req)
{
	const struct evkeyvalq *input_headers = evhttp_request_get_input_headers(req);
	const char *accept_encoding = evhttp_find_header(input_headers, "Accept-Encoding");
	if (accept_encoding) {
		size_t len = strlen(accept_encoding);
		const char *p;
		if (len > 1 && (p = strcasestr(accept_encoding, "br"))) {
			return (p == accept_encoding || *(p - 1) == ',' || *(p - 1) == ' ')
				&& (p + 2 == accept_encoding + len || *(p + 2) == ',' || *(p + 2) == ' ');
		}
	}
	return false;
}

static time_t parse_timestamp(const char *uri)
{
	time_t ret = -1;
	const char *str = uri + 1;

	if (strcmp(str, "now") == 0 || strcmp(str, "today") == 0 ||
		strcmp(str, "tomorrow") == 0 || strcmp(str, "yesterday") == 0)
		return ret;

	{
		fsec_t fsec;
		struct pg_tm tm = {0,};
		int tz, dtype, nf;
		char *field[MAXDATEFIELDS];
		int ftype[MAXDATEFIELDS];
		char workbuf[MAXDATELEN + MAXDATEFIELDS];
		TimestampTz timestamp, current_timestamp;

		size_t size_out;
		char *decoded = evhttp_uridecode(uri, 1, &size_out);

		if (decoded == NULL)
			return ret;

		if (ParseDateTime(decoded + 1, workbuf, sizeof(workbuf), field, ftype, MAXDATEFIELDS, &nf) != 0)
			goto err;

		current_timestamp = GetCurrentTimestamp();
		if (timestamp2tm(current_timestamp, NULL, &tm, &fsec, NULL, NULL) != 0)
			goto err;

		if (DecodeDateTime(field, ftype, nf, &dtype, &tm, &fsec, &tz) < 0)
			goto err;

		if (dtype != DTK_DATE && dtype != DTK_TIME)
			goto err;

		if (tm2timestamp(&tm, fsec, &tz, &timestamp) != 0)
			goto err;

		/* when request has only a time in the future off by 1 hour and more consider it as a prev day */
		if (nf == 1 && ftype[0] == DTK_TIME && (timestamp - current_timestamp) / USECS_PER_HOUR > 0) {
			j2date(date2j(tm.tm_year, tm.tm_mon, tm.tm_mday) - 1, &tm.tm_year, &tm.tm_mon, &tm.tm_mday);

			if (tm2timestamp(&tm, fsec, &tz, &timestamp) != 0)
				goto err;
		}

		ret = timestamptz_to_time_t(timestamp);
err:
		free(decoded);
		return ret;
	}
}

static bool check_bucket_timerange(time_t tm)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	tm = tv.tv_sec - tm;
	return tm > 0 && tm / 60 <= bg_mon_num_buckets;
}

static time_t parse_bucket_request(const char *uri)
{
	time_t ret = 0;
	const char *p = uri + 1;

	if (*uri != '/')
		return -1;

	do {
		if (*p < '0' || *p > '9')
			return parse_timestamp(uri);
		ret = 10 * ret + (*p - '0');
	} while (*(++p));

	if (ret >= bg_mon_num_buckets && !check_bucket_timerange(ret))
		return -1;

	return ret;
}

static time_t round_minute(time_t time)
{
	struct tm *tm = gmtime(&time);
	return time - tm->tm_sec;
}

static bool check_bucket_time(struct aggregated_stats *agg, time_t tm)
{
	if (tm)
		return round_minute(tm) == agg->minute;

	return check_bucket_timerange(agg->minute);
}

static bool send_aggregated_data(struct evhttp_request *req, struct aggregated_stats *agg, time_t tm)
{
	bool ret = false;
	struct evbuffer *evb;

	pthread_mutex_lock(&agg_lock);
	if ((ret = agg != NULL)) {
		if (check_bucket_time(agg, tm)) {
			if (!agg->state.is_flushed && !agg->state.is_finished)
				brotli_compress_data(&agg->state, NULL, 0, BROTLI_OPERATION_FLUSH);

			evb = evbuffer_new();
			evbuffer_add(evb, agg->state.to, agg->state.pos);
		} else ret = false;
	}
	pthread_mutex_unlock(&agg_lock);

	if (ret) {
		struct evkeyvalq *output_headers = evhttp_request_get_output_headers(req);
		evhttp_add_header(output_headers, "Content-Type", "application/json");
		evhttp_add_header(output_headers, "Content-Encoding", "br");
		evhttp_send_reply(req, 200, "OK", evb);
		evbuffer_free(evb);
	}
	return ret;
}
#endif

#define QUOTE(STRING) "\"" STRING "\""
#if PG_VERSION_NUM < 90500
#define MyLatch &MyProc->procLatch
#endif

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
#if PG_VERSION_NUM < 90500
	if (MyProc)
#endif
	SetLatch(MyLatch);
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
#if PG_VERSION_NUM < 90500
	if (MyProc)
#endif
	SetLatch(MyLatch);
	errno = save_errno;
}

static void
initialize_bg_mon()
{
	postgres_stats_init();
	disk_stats_init();
	system_stats_init();
	net_stats_init();
}

static void device_io_output(struct evbuffer *evb, device_stat *stats, int id)
{
	device_stat d = stats[id];
	evbuffer_add_printf(evb, "\"name\":\"%s\",\"io\":{", d.name);
	evbuffer_add_printf(evb, "\"read\":%.2f,\"reads_ps\":%.2f", d.read_diff, d.read_completed_diff);
	evbuffer_add_printf(evb, ",\"write\":%.2f,\"writes_ps\":%.2f", d.write_diff, d.write_completed_diff);
	if (HAS_EXTENDED_STATS(d)) {
		evbuffer_add_printf(evb, ",\"read_merges\":%.2f,\"write_merges\":%.2f", d.read_merges_diff, d.write_merges_diff);
		evbuffer_add_printf(evb, ",\"average_queue_length\":%.2f", d.average_queue_length);
		evbuffer_add_printf(evb, ",\"average_request_size\":%.2f", d.average_request_size);
		evbuffer_add_printf(evb, ",\"read_average_request_size\":%.2f", d.read_average_request_size);
		evbuffer_add_printf(evb, ",\"write_average_request_size\":%.2f", d.write_average_request_size);
		if (HAS_DISCARD_STATS(d))
			evbuffer_add_printf(evb, ",\"discard_average_request_size\":%.2f", d.discard_average_request_size);
		evbuffer_add_printf(evb, ",\"average_service_time\":%.2f,\"util\":%.2f", d.average_service_time, d.util);
		evbuffer_add_printf(evb, ",\"await\":%.2f,\"read_await\":%.2f,\"write_await\":%.2f", d.await, d.read_await, d.write_await);
		if (HAS_DISCARD_STATS(d))
			evbuffer_add_printf(evb, ",\"discard_await\":%.2f", d.discard_await);
		if (HAS_FLUSH_STATS(d))
			evbuffer_add_printf(evb, ",\"flush_await\":%.2f", d.flush_await);
	}
	if (d.slave_size > 0) {
		int n;
		evbuffer_add_printf(evb, ",\"slaves\":[");
		for (n = 0; n < d.slave_size; ++n) {
			if (n > 0) evbuffer_add_printf(evb, ",");
			evbuffer_add_printf(evb, "{");
			device_io_output(evb, stats, d.slaves[n]);
			evbuffer_add_printf(evb, "}");
		}
		evbuffer_add_printf(evb, "]");
	}
	evbuffer_add_printf(evb, "}");
}

static const char *process_type(pg_stat_activity p)
{

	char *backend_names[] = {
		NULL,
		QUOTE(UNKNOWN_NAME),
		QUOTE(AUTOVAC_LAUNCHER_PROC_NAME),
		QUOTE(AUTOVAC_WORKER_PROC_NAME),
		QUOTE(BACKEND_PROC_NAME),
		NULL,
		QUOTE(BG_WRITER_NAME),
		QUOTE(CHECKPOINTER_PROC_NAME),
		QUOTE(STARTUP_PROC_NAME),
		QUOTE(WAL_RECEIVER_NAME),
		QUOTE(WAL_SENDER_NAME),
		QUOTE(WAL_WRITER_NAME),
		QUOTE(ARCHIVER_PROC_NAME),
		QUOTE(STATS_COLLECTOR_PROC_NAME),
		QUOTE(LOGGER_PROC_NAME),
		QUOTE(PARALLEL_WORKER_NAME),
		QUOTE(LOGICAL_LAUNCHER_NAME),
		QUOTE(LOGICAL_WORKER_NAME)
	};

	if (p.type == PG_BG_WORKER)
		return p.ps.cmdline;

	return backend_names[p.type + 1];
}

static const char *get_query(pg_stat_activity s)
{
	if (s.type == PG_LOGICAL_WORKER)
		return s.ps.cmdline;

	switch (s.state)
	{
		case STATE_IDLE:
			return s.type == PG_BG_WORKER ? NULL : s.query ? s.query : QUOTE("idle");
		case STATE_RUNNING:
			return s.query;
		case STATE_IDLEINTRANSACTION:
			return s.idle_in_transaction_age == 0 ? QUOTE("idle in transaction") : NULL;
		case STATE_FASTPATH:
			return QUOTE("fastpath function call");
		case STATE_IDLEINTRANSACTION_ABORTED:
			return QUOTE("idle in transaction (aborted)");
		case STATE_DISABLED:
			return QUOTE("disabled");
		default:
			return s.query;
	}
}

static struct evbuffer *prepare_statistics_output(struct timeval time, system_stat s, pg_stat p, disk_stats ds, net_stats ns)
{
	struct evbuffer *evb = evbuffer_new();
	unsigned long long ts = (unsigned long long)time.tv_sec*1000 + (unsigned long long)((double)time.tv_usec/1000.0);

	pg_stat_activity_list a = p.activity;
	db_stat_list d = p.db;
	wal_metrics w = p.wal_metrics;
	cpu_stat c = s.cpu;
	meminfo m = s.mem;
	load_avg la = s.load_avg;
	pressure *p_cpu = s.p_cpu;
	pressure *p_memory = s.p_memory;
	pressure *p_io = s.p_io;

	bool is_first = true;
	size_t i;

	evbuffer_add_printf(evb, "{\"hostname\":\"%s\",\"time\":%llu,\"sysname\":\"Linux: %s\",", s.hostname, ts, s.sysname);
	evbuffer_add_printf(evb, "\"cpu_cores\":%d,\"postgresql\":{\"version\":\"%s\"", c.cpu_count, PG_VERSION);
	evbuffer_add_printf(evb, ",\"role\":\"%s\",\"wal\":{", p.recovery_in_progress?"replica":"master");

	if (p.recovery_in_progress) {
		evbuffer_add_printf(evb, "\"replay_paused\":%s,", w.is_wal_replay_paused?"true":"false");
		evbuffer_add_printf(evb, "\"replay_timestamp\":%ld,", timestamptz_to_time_t(w.last_xact_replay_timestamp));  /* unix seconds! */
		evbuffer_add_printf(evb, "\"receive\":%lu,", w.receive_diff);
		evbuffer_add_printf(evb, "\"replay\":%lu", w.replay_diff);
	} else {
		evbuffer_add_printf(evb, "\"written\":%lu", w.current_diff);
	}

	evbuffer_add_printf(evb, "},\"data_directory\":\"%s\",\"connections\":{\"max\":%d,", DataDir, MaxConnections);
	evbuffer_add_printf(evb, "\"total\":%d,\"idle_in_transaction\":%d", a.total_connections, a.idle_in_transaction_connections);
	evbuffer_add_printf(evb, ",\"active\":%d},\"start_time\":%lu},", a.active_connections, pg_start_time);
	evbuffer_add_printf(evb, "\"system_stats\":{\"uptime\":%d,\"load_average\":", (int)(s.uptime / SC_CLK_TCK));
	evbuffer_add_printf(evb, "[%.6g,%.6g,%.6g],\"cpu\":{\"user\":", la.run_1min, la.run_5min, la.run_15min);
	evbuffer_add_printf(evb, "%2.1f,\"nice\":%2.1f,\"system\":%2.1f,", c.utime_diff, c.ntime_diff, c.stime_diff);
	evbuffer_add_printf(evb, "\"idle\":%2.1f,\"iowait\":%2.1f,\"steal\":%2.1f", c.idle_diff, c.iowait_diff, c.steal_diff);
	evbuffer_add_printf(evb, "},\"ctxt\":%lu,\"processes\":{\"running\":%lu,\"blocked\": %lu}", s.ctxt_diff, s.procs_running, s.procs_blocked);

	if (s.pressure) {
		evbuffer_add_printf(evb, ",\"pressure\":{\"cpu\":[%.6g,%.6g,%.6g,%lu],",
							p_cpu[0].avg10, p_cpu[0].avg60, p_cpu[0].avg300, p_cpu[0].total);
		evbuffer_add_printf(evb, "\"memory\":{\"some\":[%.6g,%.6g,%.6g,%lu],\"full\":[%.6g,%.6g,%.6g,%lu]},",
							p_memory[0].avg10, p_memory[0].avg60, p_memory[0].avg300, p_memory[0].total,
							p_memory[1].avg10, p_memory[1].avg60, p_memory[1].avg300, p_memory[1].total);
		evbuffer_add_printf(evb, "\"io\":{\"some\":[%.6g,%.6g,%.6g,%lu],\"full\":[%.6g,%.6g,%.6g,%lu]}}",
							p_io[0].avg10, p_io[0].avg60, p_io[0].avg300, p_io[0].total,
							p_io[1].avg10, p_io[1].avg60, p_io[1].avg300, p_io[1].total);
	}

	evbuffer_add_printf(evb, ",\"memory\":{\"total\":%lu,\"free\":%lu,", m.total, m.free);
	evbuffer_add_printf(evb, "\"buffers\":%lu,\"cached\":%lu,\"dirty\":%lu", m.buffers, m.cached, m.dirty);

	if (m.overcommit.memory == 2) {
		evbuffer_add_printf(evb, ",\"overcommit\":{\"ratio\":%u,", m.overcommit.ratio);
		evbuffer_add_printf(evb, "\"commit_limit\":%lu,\"committed_as\":%lu}", m.limit, m.as);
	}

	if (m.cgroup.available || c.cgroup.available) {
		cgroup_memory cm = m.cgroup;
		cgroup_cpu cc = c.cgroup;
		evbuffer_add_printf(evb, "}},\"cgroup\":{");
		if (cm.available) {
			evbuffer_add_printf(evb, "\"memory\":{\"limit\":%lu,\"usage\":%lu,", cm.limit, cm.usage);
			evbuffer_add_printf(evb, "\"rss\":%lu,\"cache\":%lu,\"dirty\":%lu", cm.rss, cm.cache, cm.dirty);
			evbuffer_add_printf(evb, ",\"oom_kill\":%lu,\"failcnt\":%lu", cm.oom_kill, cm.failcnt);
			if (cc.available)
				evbuffer_add_printf(evb, "},");
		}
		if (cc.available) {
			evbuffer_add_printf(evb, "\"cpu\":{\"shares\":%llu,\"quota\":%lld", cc.shares, cc.quota);
			evbuffer_add_printf(evb, ",\"user\":%2.1f,\"system\":%2.1f", cc.user_diff, cc.system_diff);
		}
	}

	evbuffer_add_printf(evb, "}},\"disk_stats\":{");
	for (i = 0; i < lengthof(ds.values); ++i) {
		disk_stat device = ds.values[i];
		if (i > 0) evbuffer_add_printf(evb, ",");
		evbuffer_add_printf(evb, "\"%s\":{\"device\":{\"space\":", device.type);
		evbuffer_add_printf(evb, "{\"total\":%llu,\"left\":%llu},", device.size, device.free);
		device_io_output(evb, ds.dstats.values, device.device_id);
		evbuffer_add_printf(evb, "},\"directory\":{\"name\":\"%s\",\"size\":%llu}}", device.directory, device.du);
	}

	if (d.pos > 0) {
		evbuffer_add_printf(evb, "},\"databases\":{");
		for (i = 0; i < d.pos; ++i) {
			if (is_first) is_first = false;
			else evbuffer_add_printf(evb, ",");
			evbuffer_add_printf(evb, "%s:{", d.values[i].datname);
			evbuffer_add_printf(evb, "\"xact\":{\"commit\":%ld,", d.values[i].n_xact_commit_diff);
			evbuffer_add_printf(evb, "\"rollback\":%ld},", d.values[i].n_xact_rollback_diff);
			evbuffer_add_printf(evb, "\"blocks\":{\"fetched\":%ld,", d.values[i].n_blocks_fetched_diff);
			evbuffer_add_printf(evb, "\"hit\":%ld", d.values[i].n_blocks_hit_diff);
			if (d.track_io_timing) {
				evbuffer_add_printf(evb, ",\"read_time\":%.2f", d.values[i].n_block_read_time_diff); /* percents */
				evbuffer_add_printf(evb, ",\"write_time\":%.2f", d.values[i].n_block_write_time_diff);
			}
			evbuffer_add_printf(evb, "},\"tuples\":{\"returned\":%ld,", d.values[i].n_tuples_returned_diff);
			evbuffer_add_printf(evb, "\"fetched\":%ld,", d.values[i].n_tuples_fetched_diff);
			evbuffer_add_printf(evb, "\"updated\":%ld,", d.values[i].n_tuples_updated_diff);
			evbuffer_add_printf(evb, "\"inserted\":%ld,", d.values[i].n_tuples_inserted_diff);
			evbuffer_add_printf(evb, "\"deleted\":%ld},", d.values[i].n_tuples_deleted_diff);
			evbuffer_add_printf(evb, "\"temp\":{\"files\":%ld,", d.values[i].n_temp_files_diff);
			evbuffer_add_printf(evb, "\"bytes\":%ld},", d.values[i].n_temp_bytes_diff);
			if (p.recovery_in_progress) {
				evbuffer_add_printf(evb, "\"conflict\":{\"tablespace\":%ld,", d.values[i].n_conflict_tablespace);
				evbuffer_add_printf(evb, "\"lock\":%ld,", d.values[i].n_conflict_lock);
				evbuffer_add_printf(evb, "\"snapshot\":%ld,", d.values[i].n_conflict_snapshot);
				evbuffer_add_printf(evb, "\"bufferpin\":%ld,", d.values[i].n_conflict_bufferpin);
				evbuffer_add_printf(evb, "\"startup_deadlock\":%ld},", d.values[i].n_conflict_startup_deadlock);
			}
			evbuffer_add_printf(evb, "\"deadlocks\":%ld", d.values[i].n_deadlocks);
#if PG_VERSION_NUM >= 120000
			if (d.values[i].n_checksum_failures) {
				evbuffer_add_printf(evb, ",\"checksum_failures\":%ld,", d.values[i].n_checksum_failures);
				evbuffer_add_printf(evb, "\"last_checksum_failure\":%lu", timestamptz_to_time_t(d.values[i].last_checksum_failure));
			}
#endif
			evbuffer_add_printf(evb, "}");
		}
	}

	is_first = true;
	evbuffer_add_printf(evb, "},\"net_stats\":{");
	for (i = 0; i < ns.size; ++i)
		if (ns.values[i].is_used && ns.values[i].has_statistics) {
			net_stat n = ns.values[i];
			if (is_first) is_first = false;
			else evbuffer_add_printf(evb, ",");
			evbuffer_add_printf(evb, "\"%s\":{\"rx_kbytes\":%.2f,", n.name, n.rx_bytes_diff / 1024.0);
			evbuffer_add_printf(evb, "\"rx_packets\":%.2f,", n.rx_packets_diff);
			evbuffer_add_printf(evb, "\"rx_errors\":%.2f,", n.rx_errors_diff);
			evbuffer_add_printf(evb, "\"rx_util\":%.2f,", n.rx_util);
			evbuffer_add_printf(evb, "\"tx_kbytes\":%.2f,", n.tx_bytes_diff / 1024.0);
			evbuffer_add_printf(evb, "\"tx_packets\":%.2f,", n.tx_packets_diff);
			evbuffer_add_printf(evb, "\"tx_errors\":%.2f,", n.tx_errors_diff);
			evbuffer_add_printf(evb, "\"tx_util\":%.2f,", n.tx_util);
			evbuffer_add_printf(evb, "\"util\":%.2f,", n.util);
			evbuffer_add_printf(evb, "\"saturation\":%.2f,", n.saturation_diff);
			evbuffer_add_printf(evb, "\"collisions\":%.2f}", n.collisions_diff);
		}

	is_first = true;
	evbuffer_add_printf(evb, "},\"processes\":[");
	for (i = 0; i < a.pos; ++i) {
		pg_stat_activity s = a.values[i];
		if (s.type != PG_BACKEND || s.query != NULL || s.state >= STATE_RUNNING || s.is_blocker) {
			proc_stat ps = s.ps;
			proc_io io = ps.io;
			const char *tmp = process_type(s);
			if (tmp == NULL || *tmp == '\0') continue;
			if (is_first) is_first = false;
			else evbuffer_add_printf(evb, ",");
			evbuffer_add_printf(evb, "{\"pid\":%d,\"type\":%s,\"state\":\"%c\",", s.pid, tmp, ps.state ? ps.state : 'S');
			evbuffer_add_printf(evb, "\"cpu\":{\"user\":%2.1f,\"system\":%2.1f,", ps.utime_diff, ps.stime_diff);
			evbuffer_add_printf(evb, "\"guest\":%2.1f},\"io\":{\"read\":%lu,", ps.gtime_diff, io.read_diff);
			evbuffer_add_printf(evb, "\"write\":%lu},\"uss\":%llu", io.write_diff, ps.uss);
			if (s.type == PG_BACKEND || s.type == PG_AUTOVAC_WORKER || s.type == PG_PARALLEL_WORKER) {
				if (s.type == PG_PARALLEL_WORKER && s.parent_pid > 0)
					evbuffer_add_printf(evb, ",\"parent_pid\":%d", s.parent_pid);

				if (s.num_blockers > 0) {
					int j;
					evbuffer_add_printf(evb, ",\"locked_by\":[%d", s.num_blockers == 1 ? (uint32)s.blockers : *(uint32 *)s.blockers);
					for (j = 1; j < s.num_blockers; ++j)
						evbuffer_add_printf(evb, ",%d", ((uint32 *)s.blockers)[j]);
					evbuffer_add_printf(evb, "]");
				}

				if (s.age > -1) {
					if (s.age < 10)
						evbuffer_add_printf(evb, ",\"age\":%.2g", s.age);
					else
						evbuffer_add_printf(evb, ",\"age\":%ld", (long)s.age);
				}
			}

			if (s.datname != NULL || s.type == PG_BACKEND)
				evbuffer_add_printf(evb, ",\"database\":%s", s.datname == NULL ? "null" : s.datname);
			if (s.usename != NULL || s.type == PG_BACKEND)
				evbuffer_add_printf(evb, ",\"username\":%s", s.usename == NULL ? "null" : s.usename);
#if PG_VERSION_NUM >= 90600
			if (s.raw_wait_event) {
				evbuffer_add_printf(evb, ",\"wait_event_type\":\"%s\"", pgstat_get_wait_event_type(s.raw_wait_event));
				evbuffer_add_printf(evb, ",\"wait_event\":\"%s\"", pgstat_get_wait_event(s.raw_wait_event));
			}
#endif
			if (s.state == STATE_IDLEINTRANSACTION && s.idle_in_transaction_age > 0) {
				if (s.idle_in_transaction_age < 10)
					evbuffer_add_printf(evb, ",\"query\":\"idle in transaction %.2g\"", s.idle_in_transaction_age);
				else
					evbuffer_add_printf(evb, ",\"query\":\"idle in transaction %ld\"", (long)s.idle_in_transaction_age);
			} else if ((tmp = get_query(s)) != NULL)
				evbuffer_add_printf(evb, ",\"query\":%s", tmp);
			evbuffer_add_printf(evb, "}");
		}
	}

	evbuffer_add_printf(evb, "]}");
	return evb;
}

static void send_document_cb(struct evhttp_request *req, void *arg)
{
	bool err = false;

	const char *uri = evhttp_request_get_uri(req);
	struct evkeyvalq *output_headers = evhttp_request_get_output_headers(req);
#ifdef HAS_LIBBROTLI
	time_t tm = parse_bucket_request(uri);
	if (tm > -1) {
		int bucket;
		if (tm < bg_mon_num_buckets) {
			bucket = tm;
			tm = 0;
		} else
			bucket = (tm / 60) % bg_mon_num_buckets;
		err = !send_aggregated_data(req, aggregated + bucket, tm);
	} else if (strcmp(uri, "/prev") == 0)
		err = !send_aggregated_data(req, prev, 0);
	else
#endif
	if (strncmp(uri, "/ui", 3)) {
		pthread_mutex_lock(&lock);
		if (evbuffer) {
			struct evbuffer *evb = evbuffer_new();
#if HAS_LIBBROTLI
			if (accepts_brotli(req)) {
				struct compression_state *state = malloc_fn(sizeof(struct compression_state));
				state->to = NULL;
				state->len = 0;
				brotli_init(state, 7);
				brotli_compress_evbuffer(state, evbuffer, BROTLI_OPERATION_FINISH);
				evbuffer_add_reference(evb, state->to, state->pos, brotli_cleanup, state);
				evhttp_add_header(output_headers, "Content-Encoding", "br");
			} else
#endif
			{
				struct evbuffer_iovec v_out[1];
				evbuffer_reserve_space(evb, evbuffer_get_length(evbuffer), v_out, 1);
				v_out[0].iov_len = evbuffer_copyout(evbuffer, v_out[0].iov_base, v_out[0].iov_len);
				evbuffer_commit_space(evb, v_out, 1);
			}
			evhttp_add_header(output_headers, "Content-Type", "application/json");
			evhttp_send_reply(req, 200, "OK", evb);
			evbuffer_free(evb);
		} else err = true;
		pthread_mutex_unlock(&lock);
	} else {
		int fd = open(UIFILE, O_RDONLY);
		if (!(err = (fd < 0))) {
			struct stat st;
			if (!(err = (fstat(fd, &st) < 0))) {
				struct evbuffer *evb = evbuffer_new();
				evhttp_add_header(output_headers, "Content-Type", "text/html");
				evbuffer_add_file(evb, fd, 0, st.st_size);
				evhttp_send_reply(req, 200, "OK", evb);
				evbuffer_free(evb);
			} else close(fd);
		}
	}

	if (err) evhttp_send_error(req, 404, "Document was not found");
}

static void *webapi(void *arg)
{
	struct event_base *base = (struct event_base *)arg;

	event_base_dispatch(base);

	return (void *)0;
}

#ifdef HAS_LIBBROTLI
static void update_aggregated_statistics(time_t time)
{
	static struct aggregated_stats *current = NULL;
	time_t minute = round_minute(time);
	struct aggregated_stats *agg = aggregated + ((minute/60) % bg_mon_num_buckets);

	pthread_mutex_lock(&agg_lock);

	if (current == agg && current->minute != minute) {
		brotli_dealloc(&current->state);
		prev = current = NULL;
	}

	if (agg != current) {
		if (current != NULL) {
			brotli_compress_data(&current->state, (const unsigned char *)"]", 1, BROTLI_OPERATION_FINISH);
			prev = current;
		}
		current = agg;
		brotli_init(&agg->state, 7);
		brotli_compress_data(&agg->state, (const unsigned char *)"[", 1, BROTLI_OPERATION_PROCESS);
		agg->minute = minute;
	} else
		brotli_compress_data(&agg->state, (const unsigned char *)",", 1, BROTLI_OPERATION_PROCESS);

	brotli_compress_evbuffer(&agg->state, evbuffer, BROTLI_OPERATION_PROCESS);

	pthread_mutex_unlock(&agg_lock);
}
#endif

static void update_statistics(struct timeval time)
{
	system_stat s = get_system_stats();
	pg_stat p = get_postgres_stats();
	disk_stats d =  get_disk_stats();
	net_stats n = get_net_stats();

	struct evbuffer *evb = prepare_statistics_output(time, s, p, d, n);
	struct evbuffer *old;

	pthread_mutex_lock(&lock);

	old = evbuffer;
	evbuffer = evb;

	pthread_mutex_unlock(&lock);

	if (old != NULL)
		evbuffer_free(old);
#ifdef HAS_LIBBROTLI
	if (bg_mon_num_buckets > 0)
		update_aggregated_statistics(time.tv_sec);
#endif
}

void
bg_mon_main(Datum main_arg)
{
	pthread_t thread;
	struct timezone tz;
	struct timeval current_time, next_run;

	struct event_base *base;
	struct evhttp *http;
	struct evhttp_bound_socket *handle;

#if PG_VERSION_NUM >= 90600
	MemoryContext bg_mon_cxt = AllocSetContextCreate(TopMemoryContext, "BgMon", ALLOCSET_SMALL_SIZES);
#else
	MemoryContext bg_mon_cxt = AllocSetContextCreate(TopMemoryContext, "BgMon", ALLOCSET_SMALL_MINSIZE,
													ALLOCSET_SMALL_INITSIZE, ALLOCSET_SMALL_MAXSIZE);
#endif
	MemoryContextSwitchTo(bg_mon_cxt);

#ifdef HAS_LIBBROTLI
	aggregated = palloc0(bg_mon_num_buckets * sizeof(struct aggregated_stats));
#endif

	pg_start_time = timestamptz_to_time_t(PgStartTime);

	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, bg_mon_sighup);
	pqsignal(SIGTERM, bg_mon_sigterm);

	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		proc_exit(1);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	initialize_bg_mon();
	evthread_use_pthreads();
	event_set_mem_functions(malloc_fn, realloc_fn, free);

restart:
	FREE(bg_mon_listen_address);
	bg_mon_listen_address = palloc(strlen(bg_mon_listen_address_guc) + 1);
	strcpy(bg_mon_listen_address, bg_mon_listen_address_guc);

	bg_mon_port = bg_mon_port_guc;

	if (!(base = event_base_new()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("Couldn't create an event_base"),
				 errdetail("event_base_new() returned NULL")));

	/* Create a new evhttp object to handle requests. */
	if (!(http = evhttp_new(base)))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("Couldn't create an evhttp"),
				 errdetail("evhttp_new() returned NULL")));

	/* We want to accept arbitrary requests, so we need to set a "generic"
	 * cb.  We can also add callbacks for specific paths. */
	evhttp_set_gencb(http, send_document_cb, NULL);

	/* Now we tell the evhttp what port to listen on */
	if (!(handle = evhttp_bind_socket_with_handle(http, bg_mon_listen_address, bg_mon_port)))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("evhttp_bind_socket_with_handle() failed"),
				 errdetail("couldn't bind to %s:%d", bg_mon_listen_address, bg_mon_port)));

	pthread_mutex_init(&lock, NULL);
#ifdef HAS_LIBBROTLI
	pthread_mutex_init(&agg_lock, NULL);
#endif

	pthread_create(&thread, NULL, webapi, base);

	gettimeofday(&next_run, &tz);

	/*
	 * Main loop: do this until the SIGTERM handler tells us to terminate
	 */
	while (!got_sigterm) {
		int		rc;
		double	naptime;

		next_run.tv_sec += bg_mon_naptime_guc; /* adjust wakeup target time */

		gettimeofday(&current_time, &tz);

		naptime = 1000L * (
			(double)next_run.tv_sec + (double)next_run.tv_usec/1000000.0 -
			(double)current_time.tv_sec - (double)current_time.tv_usec/1000000.0
		);

		if (naptime <= 0) { // something is very slow
			next_run = current_time; // reschedule next run
			naptime = 0;
		}
#if PG_VERSION_NUM >= 100000
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   naptime,
					   PG_WAIT_EXTENSION);
#else
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   naptime);
#endif
		ResetLatch(MyLatch);
		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		/*
		 * In case of a SIGHUP, reload the configuration and restart the web server.
		 */
		if (got_sighup) {
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);

			if (strcmp(bg_mon_listen_address_guc, bg_mon_listen_address) != 0
					|| bg_mon_port_guc != bg_mon_port) {
				struct timeval delay = {0, 0};

				event_base_loopexit(base, &delay);
//				pthread_cancel(thread);
				pthread_join(thread, NULL);

				pthread_mutex_destroy(&lock);
#ifdef HAS_LIBBROTLI
				pthread_mutex_destroy(&agg_lock);
#endif
				evhttp_free(http);
				event_base_free(base);
				goto restart;
			}
		}

		update_statistics(next_run);
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

	/* get the configuration */
	DefineCustomIntVariable("bg_mon.naptime",
							"Duration between each run (in seconds).",
							NULL,
							&bg_mon_naptime_guc,
							1,
							1,
							10,
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL,
							NULL,
							NULL);
	DefineCustomStringVariable("bg_mon.listen_address",
							"The IP address for the web server to listen on.",
							NULL,
							&bg_mon_listen_address_guc,
							"127.0.0.1",
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);
	DefineCustomIntVariable("bg_mon.port",
							"Port number to bind the web server to.",
							NULL,
							&bg_mon_port_guc,
							8080,
							0,
							65535,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);
#ifdef HAS_LIBBROTLI
	DefineCustomIntVariable("bg_mon.history_buckets",
							"The number of buckets to keep aggregated historic data",
							NULL,
							&bg_mon_num_buckets,
							20,
							0,
							1440,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);
#endif

	if (!process_shared_preload_libraries_in_progress)
		return;

	/* set up common data for all our workers */
	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_PostmasterStart;
	worker.bgw_restart_time = 1;
#if PG_VERSION_NUM >= 100000
	sprintf(worker.bgw_library_name, "bg_mon");
	sprintf(worker.bgw_function_name, "bg_mon_main");
#else
	worker.bgw_main = bg_mon_main;
#endif
#if PG_VERSION_NUM >= 90400
	worker.bgw_notify_pid = 0;
#endif

	/*
	 * Now fill in worker-specific data, and do the actual registrations.
	 */
	snprintf(worker.bgw_name, BGW_MAXLEN, "bg_mon");

	RegisterBackgroundWorker(&worker);
}
