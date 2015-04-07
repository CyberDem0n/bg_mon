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

#include "postgres_stats.h"
#include "disk_stats.h"
#include "system_stats.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(bg_mon_launch);

void _PG_init(void);
void bg_mon_main(Datum);


/* flags set by signal handlers */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* GUC variables */
static int bg_mon_naptime = 1;

static pg_stat_list pg_stats_current;
static disk_stat diskspace_stats_current;
static system_stat system_stats_current;

static void report_stats()
{
	size_t i;
	disk_stat d = diskspace_stats_current;
	system_stat s = system_stats_current;
	cpu_stat c = s.cpu;
	meminfo m = s.mem;
	elog(LOG, "%s up %d %d cores Linux %s load average %4.6g %4.6g %4.6g", s.hostname, s.uptime, s.cpu.cpu_count, s.sysname, s.load_avg.run_1min, s.load_avg.run_5min, s.load_avg.run_15min);
	elog(LOG, "sys: utime %2.1f stime %2.1f idle %2.1f iowait %2.1f ctxt %lu run %lu block %lu", c.utime_diff*100, c.stime_diff*100, c.idle_diff*100, c.iowait_diff*100, s.ctxt_diff, s.procs_running, s.procs_blocked);
	elog(LOG, "mem: total %lu free %lu buffers %lu cached %lu dirty %lu limit %lu as %lu", m.total, m.free, m.buffers, m.cached, m.dirty, m.limit, m.as);
	elog(LOG, "data %s:%s total %llu left %llu size %llu read %u write %u await %u", d.data_dev, d.data_directory, d.data_size, d.data_free, d.du_data, d.data_read_diff, d.data_write_diff, d.data_time_in_queue_diff);
	elog(LOG, "xlog %s:%s total %llu left %llu size %llu read %u write %u await %u", d.xlog_dev, d.xlog_directory, d.xlog_size, d.xlog_free, d.du_xlog, d.xlog_read_diff, d.xlog_write_diff, d.xlog_time_in_queue_diff);
	for (i = 0; i < pg_stats_current.pos; ++i) {
		pg_stat s = pg_stats_current.values[i];
		proc_stat ps = s.ps;
		if (s.is_backend && s.query != NULL)
			elog(LOG, "%d %s %c %f %f %f %lu %lu %s %s %s", s.pid, "backend", ps.state, ps.utime_diff, ps.stime_diff, ps.guest_time_diff, ps.io.read_bytes_diff, ps.io.write_bytes_diff, s.datname==NULL?"nil":s.datname, s.usename==NULL?"nil":s.usename, s.query);
		else if (!s.is_backend)
			elog(LOG, "%d %s %c %f %f %f %lu %lu", s.pid, ps.cmdline==NULL?"unknown":ps.cmdline, ps.state, ps.utime_diff, ps.stime_diff, ps.guest_time_diff, ps.io.read_bytes_diff, ps.io.write_bytes_diff);
	}
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
	postgres_stats_init();
	disk_stats_init();
	system_stats_init();
}

void
bg_mon_main(Datum main_arg)
{
	struct timezone tz;
	struct timeval current_time, next_run;

	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, bg_mon_sighup);
	pqsignal(SIGTERM, bg_mon_sigterm);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
	BackgroundWorkerInitializeConnection("postgres", NULL);

	initialize_bg_mon();

	gettimeofday(&next_run, &tz);

	/*
	 * Main loop: do this until the SIGTERM handler tells us to terminate
	 */
	while (!got_sigterm) {
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
			int rc = WaitLatch(&MyProc->procLatch,
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
		if (got_sighup) {
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		diskspace_stats_current = get_diskspace_stats();
		pg_stats_current = get_postgres_stats();
		system_stats_current = get_system_stats();

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
	snprintf(worker.bgw_name, BGW_MAXLEN, "bg_mon");
//	worker.bgw_main_arg = Int32GetDatum(i);

	RegisterBackgroundWorker(&worker);
}

/*
 * Dynamically launch an SPI worker.
 */
Datum
bg_mon_launch(PG_FUNCTION_ARGS)
{
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
	snprintf(worker.bgw_name, BGW_MAXLEN, "bg_mon");
//	worker.bgw_main_arg = Int32GetDatum(i);
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
