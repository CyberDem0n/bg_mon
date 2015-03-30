#include <unistd.h>
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

void		_PG_init(void);
void		bg_mon_main(Datum);

extern int MaxConnections;
extern int max_worker_processes;
static unsigned long long mem_page_size;

/* flags set by signal handlers */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* GUC variables */
static int	bg_mon_naptime = 1;

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
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());
	pgstat_report_activity(STATE_RUNNING, "initializing bg_mon");

	mem_page_size = getpagesize();

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();
	pgstat_report_activity(STATE_IDLE, NULL);

	elog(LOG, "max_connections=%d, max_worker_processes=%d", MaxConnections, max_worker_processes);
}

void
bg_mon_main(Datum main_arg)
{
	StringInfoData buf;

	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, bg_mon_sighup);
	pqsignal(SIGTERM, bg_mon_sigterm);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
	BackgroundWorkerInitializeConnection("postgres", NULL);

	initialize_bg_mon();

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
							"(array_agg(other.pid ORDER BY other.pid))[1] as locked_by,"
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
					"pid IN (SELECT DISTINCT(locked_by) FROM activity WHERE locked_by IS NOT NULL) AS is_locker"
					" FROM activity");

	/*
	 * Main loop: do this until the SIGTERM handler tells us to terminate
	 */
	while (!got_sigterm)
	{
		int			ret;
		int			rc;

		/*
		 * Background workers mustn't call usleep() or any direct equivalent:
		 * instead, they may wait on their process latch, which sleeps as
		 * necessary, but is awakened if postmaster dies.  That way the
		 * background process goes away immediately in an emergency.
		 */
		rc = WaitLatch(&MyProc->procLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
					   bg_mon_naptime * 1000L);
		ResetLatch(&MyProc->procLatch);

		/* emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		/*
		 * In case of a SIGHUP, just reload the configuration.
		 */
		if (got_sighup)
		{
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/*
		 * Start a transaction on which we can run queries.  Note that each
		 * StartTransactionCommand() call should be preceded by a
		 * SetCurrentStatementStartTimestamp() call, which sets both the time
		 * for the statement we're about the run, and also the transaction
		 * start time.  Also, each other query sent to SPI should probably be
		 * preceded by SetCurrentStatementStartTimestamp(), so that statement
		 * start time is always up to date.
		 *
		 * The SPI_connect() call lets us run queries through the SPI manager,
		 * and the PushActiveSnapshot() call creates an "active" snapshot
		 * which is necessary for queries to have MVCC data to work on.
		 *
		 * The pgstat_report_activity() call makes our activity visible
		 * through the pgstat views.
		 */
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
			char *datname, *usename, *query;
			int a;
			bool		isnull, islocker, waiting;
			int32		pid, age, locked_by;

			for (a = 0; a < SPI_processed; a++) {
				query = SPI_getvalue(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 9);
				islocker = DatumGetBool(SPI_getbinval(SPI_tuptable->vals[a],
													SPI_tuptable->tupdesc,
													10, &isnull));
				if (!islocker || !strncmp(query, "idle", 5)) continue;

				datname = SPI_getvalue(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 1);
				pid = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[a],
													SPI_tuptable->tupdesc,
													2, &isnull));

				usename = SPI_getvalue(SPI_tuptable->vals[a], SPI_tuptable->tupdesc, 3);

				age = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[a],
													SPI_tuptable->tupdesc,
													6, &isnull));
				if (isnull) age = -1;

				waiting = DatumGetBool(SPI_getbinval(SPI_tuptable->vals[a],
													SPI_tuptable->tupdesc,
													7, &isnull));

				locked_by = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[a],
													SPI_tuptable->tupdesc,
													8, &isnull));
				if (isnull) locked_by = -1;

/*				if (!isnull)
					elog(LOG, "%s: pid%d %d query='%s'",
						 MyBgworkerEntry->bgw_name, a, pid, query);*/
			}
		}

		/*
		 * And finish our transaction.
		 */
		SPI_finish();
		PopActiveSnapshot();
		CommitTransactionCommand();
		pgstat_report_activity(STATE_IDLE, NULL);
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
