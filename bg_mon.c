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
#include "utils/timestamp.h"

#include "net_stats.h"
#include "postgres_stats.h"
#include "disk_stats.h"
#include "system_stats.h"

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

static pg_stat_list pg_stats_current;
static disk_stats disk_stats_current;
static system_stat system_stats_current;
static net_stats net_stats_current;

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

/*
 * Initialize workspace for a worker process: create the schema if it doesn't
 * already exist.
 */
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
	evbuffer_add_printf(evb, "\"name\": \"%s\", \"io\": {", d.name);
	evbuffer_add_printf(evb, "\"read\": %.2f, \"reads_ps\": %.2f", d.read_diff, d.read_completed_diff);
	evbuffer_add_printf(evb, ", \"write\": %.2f, \"writes_ps\": %.2f", d.write_diff, d.write_completed_diff);
	if (d.extended) {
		evbuffer_add_printf(evb, ", \"read_merges\": %.2f, \"write_merges\": %.2f", d.read_merges_diff, d.write_merges_diff);
		evbuffer_add_printf(evb, ", \"average_queue_length\": %.2f", d.average_queue_length);
		evbuffer_add_printf(evb, ", \"average_request_size\": %.2f", d.average_request_size);
		evbuffer_add_printf(evb, ", \"average_service_time\": %.2f", d.average_service_time);
		evbuffer_add_printf(evb, ", \"await\": %.2f, \"read_await\": %.2f", d.await, d.read_await);
		evbuffer_add_printf(evb, ", \"write_await\": %.2f, \"util\": %.2f", d.write_await, d.util);
	}
	if (d.slave_size > 0) {
		int n;
		evbuffer_add_printf(evb, ", \"slaves\": [");
		for (n = 0; n < d.slave_size; ++n) {
			if (n > 0) evbuffer_add_printf(evb, ", ");
			evbuffer_add_printf(evb, "{");
			device_io_output(evb, stats, d.slaves[n]);
			evbuffer_add_printf(evb, "}");
		}
		evbuffer_add_printf(evb, "]");
	}
	evbuffer_add_printf(evb, "}");
}

static const char *process_type(pg_stat p)
{

	char *backend_names[] = {
		NULL,
		QUOTE(UNKNOWN_PROC_NAME),
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
		QUOTE(LOGGER_PROC_NAME),
		QUOTE(STATS_COLLECTOR_PROC_NAME),
		QUOTE(LOGICAL_LAUNCHER_NAME),
		QUOTE(LOGICAL_WORKER_NAME)
	};

	if (p.type == PG_BG_WORKER)
		return p.ps.cmdline;

	return backend_names[p.type + 2];
}

static const char *get_query(pg_stat s)
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
			return NULL;
	}
}

static void prepare_statistics_output(struct evbuffer *evb)
{
	bool is_first = true;
	size_t i;
	disk_stats ds;
	system_stat s;
	net_stats ns;
	cpu_stat c;
	meminfo m;
	load_avg la;

	pthread_mutex_lock(&lock);
	ds = disk_stats_current;
	s = system_stats_current;
	ns = net_stats_current;
	c = s.cpu;
	m = s.mem;
	la = s.load_avg;

	evbuffer_add_printf(evb, "{\"hostname\": \"%s\", \"sysname\": \"Linux: %s\", ", s.hostname, s.sysname);
	evbuffer_add_printf(evb, "\"cpu_cores\": %d, \"postgresql\": {\"version\": \"%s\"", c.cpu_count, PG_VERSION);
	evbuffer_add_printf(evb, ", \"role\": \"%s\", ", pg_stats_current.recovery_in_progress?"replica":"master");
	evbuffer_add_printf(evb, "\"data_directory\": \"%s\", \"connections\": {\"max\": %d", DataDir, MaxConnections);
	evbuffer_add_printf(evb, ", \"total\": %d, ", pg_stats_current.total_connections);
	evbuffer_add_printf(evb, "\"active\": %d}, \"start_time\": %lu}, ", pg_stats_current.active_connections, pg_start_time);
	evbuffer_add_printf(evb, "\"system_stats\": {\"uptime\": %d, \"load_average\": ", (int)(s.uptime / SC_CLK_TCK));
	evbuffer_add_printf(evb, "[%4.6g, %4.6g, %4.6g], \"cpu\": ", la.run_1min, la.run_5min, la.run_15min);
	evbuffer_add_printf(evb, "{\"user\": %2.1f, \"system\": %2.1f, \"idle\": ", c.utime_diff, c.stime_diff);
	evbuffer_add_printf(evb, "%2.1f, \"iowait\": %2.1f}, \"ctxt\": %lu", c.idle_diff, c.iowait_diff, s.ctxt_diff);
	evbuffer_add_printf(evb, ", \"processes\": {\"running\": %lu, \"blocked\": %lu}, ", s.procs_running, s.procs_blocked);
	evbuffer_add_printf(evb, "\"memory\": {\"total\": %lu, \"free\": %lu, ", m.total, m.free);
	evbuffer_add_printf(evb, "\"buffers\": %lu, \"cached\": %lu, \"dirty\": %lu", m.buffers, m.cached, m.dirty);

	if (m.overcommit.memory == 2) {
		evbuffer_add_printf(evb, ", \"overcommit\": {\"ratio\": %u, ", m.overcommit.ratio);
		evbuffer_add_printf(evb, "\"commit_limit\": %lu, \"committed_as\": %lu}", m.limit, m.as);
	}

	if (m.cgroup.available) {
		cgroup_memory cm = m.cgroup;
		evbuffer_add_printf(evb, ", \"cgroup\": {\"limit\": %lu, \"usage\": %lu", cm.limit, cm.usage);
		evbuffer_add_printf(evb, ", \"rss\": %lu, \"cache\": %lu}", cm.rss, cm.cache);
	}

	evbuffer_add_printf(evb, "}}, \"disk_stats\": {");
	for (i = 0; i < lengthof(ds.values); ++i) {
		disk_stat device = ds.values[i];
		if (i > 0) evbuffer_add_printf(evb, ", ");
		evbuffer_add_printf(evb, "\"%s\": {\"device\": {\"space\": ", device.type);
		evbuffer_add_printf(evb, "{\"total\": %llu, \"left\": %llu}, ", device.size, device.free);
		device_io_output(evb, ds.dstats.values, device.device_id);
		evbuffer_add_printf(evb, "}, \"directory\": {\"name\": \"%s\", \"size\": %llu}}", device.directory, device.du);
	}

	evbuffer_add_printf(evb, "}, \"net_stats\": {");
	for (i = 0; i < ns.size; ++i)
		if (ns.values[i].is_used && ns.values[i].has_statistics) {
			net_stat n = ns.values[i];
			if (is_first) is_first = false;
			else evbuffer_add_printf(evb, ", ");
			evbuffer_add_printf(evb, "\"%s\": {\"rx_kbytes\": %.2f, ", n.name, n.rx_bytes_diff / 1024.0);
			evbuffer_add_printf(evb, "\"rx_packets\": %.2f, ", n.rx_packets_diff);
			evbuffer_add_printf(evb, "\"rx_errors\": %.2f, ", n.rx_errors_diff);
			evbuffer_add_printf(evb, "\"rx_util\": %.2f, ", n.rx_util);
			evbuffer_add_printf(evb, "\"tx_kbytes\": %.2f, ", n.tx_bytes_diff / 1024.0);
			evbuffer_add_printf(evb, "\"tx_packets\": %.2f, ", n.tx_packets_diff);
			evbuffer_add_printf(evb, "\"tx_errors\": %.2f, ", n.tx_errors_diff);
			evbuffer_add_printf(evb, "\"tx_util\": %.2f, ", n.tx_util);
			evbuffer_add_printf(evb, "\"util\": %.2f, ", n.util);
			evbuffer_add_printf(evb, "\"saturation\": %.2f, ", n.saturation_diff);
			evbuffer_add_printf(evb, "\"collisions\": %.2f}", n.collisions_diff);
		}

	is_first = true;
	evbuffer_add_printf(evb, "}, \"processes\": [");
	for (i = 0; i < pg_stats_current.pos; ++i) {
		pg_stat s = pg_stats_current.values[i];
		if (s.type != PG_BACKEND || s.query != NULL || s.is_blocker) {
			proc_stat ps = s.ps;
			proc_io io = ps.io;
			const char *tmp = process_type(s);
			if (tmp == NULL || *tmp == '\0') continue;
			if (is_first) is_first = false;
			else evbuffer_add_printf(evb, ", ");
			evbuffer_add_printf(evb, "{\"pid\": %d, \"type\": %s, \"state\": \"%c\", ", s.pid, tmp, ps.state ? ps.state : 'S');
			evbuffer_add_printf(evb, "\"cpu\": {\"user\": %2.1f, \"system\": %2.1f, ", ps.utime_diff, ps.stime_diff);
			evbuffer_add_printf(evb, "\"guest\": %2.1f}, \"io\": {\"read\": %lu, ", ps.gtime_diff, io.read_diff);
			evbuffer_add_printf(evb, "\"write\": %lu}, \"uss\": %llu", io.write_diff, ps.uss);
			if (s.type == PG_BACKEND || s.type == PG_AUTOVAC_WORKER) {
				if (s.num_blockers > 0) {
					int j;
					evbuffer_add_printf(evb, ", \"locked_by\": [%d", s.blocking_pids[0]);
					for (j = 1; j < s.num_blockers; ++j)
						evbuffer_add_printf(evb, ",%d", s.blocking_pids[j]);
					evbuffer_add_printf(evb, "]");
				}

				if (s.age > -1)
					evbuffer_add_printf(evb, ", \"age\": %.3g", s.age);
			}

			if (s.datname != NULL || s.type == PG_BACKEND)
				evbuffer_add_printf(evb, ", \"database\": %s", s.datname == NULL ? "null" : s.datname);
			if (s.usename != NULL || s.type == PG_BACKEND)
				evbuffer_add_printf(evb, ", \"username\": %s", s.usename == NULL ? "null" : s.usename);

			if (s.state == STATE_IDLEINTRANSACTION && s.idle_in_transaction_age > 0)
				evbuffer_add_printf(evb, ", \"query\": \"idle in transaction %.3g\"", s.idle_in_transaction_age);
			else if ((tmp = get_query(s)) != NULL)
				evbuffer_add_printf(evb, ", \"query\": %s", tmp);
			evbuffer_add_printf(evb, "}");
		}
	}

	evbuffer_add_printf(evb, "]}");

	pthread_mutex_unlock(&lock);
}

static void send_document_cb(struct evhttp_request *req, void *arg)
{
	bool err = false;

	const char *uri = evhttp_request_get_uri(req);

	struct evbuffer *evb = evbuffer_new();

	if (strncmp(uri, "/ui", 3)) {
		if (!(err = system_stats_current.uptime == 0)) {
			prepare_statistics_output(evb);
			evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
		}
	} else {
		int fd = open(UIFILE, O_RDONLY);
		if (!(err = (fd < 0))) {
			struct stat st;
			if (!(err = (fstat(fd, &st) < 0))) {
				evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "text/html");
				evbuffer_add_file(evb, fd, 0, st.st_size);
			} else close(fd);
		}
	}

	if (err) evhttp_send_error(req, 404, "Document was not found");
	else evhttp_send_reply(req, 200, "OK", evb);

	evbuffer_free(evb);
}

static void *webapi(void *arg)
{
	struct event_base *base = (struct event_base *)arg;

	event_base_dispatch(base);

	return (void *)0;
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

restart:
	FREE(bg_mon_listen_address);
	if (!(bg_mon_listen_address = palloc(strlen(bg_mon_listen_address_guc) + 1))) {
		elog(ERROR, "Couldn't allocate memory for bg_mon_listen_address: exiting");
		return proc_exit(1);
	}

	strcpy(bg_mon_listen_address, bg_mon_listen_address_guc);
	bg_mon_port = bg_mon_port_guc;

	if (!(base = event_base_new())) {
		elog(ERROR, "Couldn't create an event_base: exiting");
		return proc_exit(1);
	}

	/* Create a new evhttp object to handle requests. */
	if (!(http = evhttp_new(base))) {
		elog(ERROR, "couldn't create evhttp. Exiting.");
		return proc_exit(1);
	}

	/* We want to accept arbitrary requests, so we need to set a "generic"
	 * cb.  We can also add callbacks for specific paths. */
	evhttp_set_gencb(http, send_document_cb, NULL);

	/* Now we tell the evhttp what port to listen on */
	if (!(handle = evhttp_bind_socket_with_handle(http, bg_mon_listen_address, bg_mon_port))) {
		elog(ERROR, "couldn't bind to %s:%d. Exiting.", bg_mon_listen_address, bg_mon_port);
		return proc_exit(1);
	}

	pthread_mutex_init(&lock, NULL);

	pthread_create(&thread, NULL, webapi, base);

	gettimeofday(&next_run, &tz);

	/*
	 * Main loop: do this until the SIGTERM handler tells us to terminate
	 */
	while (!got_sigterm) {
		double	naptime;

		next_run.tv_sec += bg_mon_naptime_guc; /* adjust wakeup target time */

		gettimeofday(&current_time, &tz);

		naptime = 1000L * (
			(double)next_run.tv_sec + (double)next_run.tv_usec/1000000.0 -
			(double)current_time.tv_sec - (double)current_time.tv_usec/1000000.0
		);

		if (naptime <= 0) { // something is very slow
			next_run = current_time; // reschedule next run
		} else {
#if PG_VERSION_NUM >= 100000
			int rc = WaitLatch(MyLatch,
						   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
						   naptime,
						   PG_WAIT_EXTENSION);
#else
			int rc = WaitLatch(MyLatch,
						   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
						   naptime);
#endif
			ResetLatch(MyLatch);
			/* emergency bailout if postmaster has died */
			if (rc & WL_POSTMASTER_DEATH)
				proc_exit(1);
		}

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

				evhttp_free(http);
				event_base_free(base);
				goto restart;
			}
		}

		{
			system_stat s = get_system_stats();
			pg_stat_list p = get_postgres_stats();
			disk_stats d = get_disk_stats();
			net_stats n = get_net_stats();

			pthread_mutex_lock(&lock);

			system_stats_current = s;
			pg_stats_current = p;
			disk_stats_current = d;
			net_stats_current = n;

			pthread_mutex_unlock(&lock);
		}
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
