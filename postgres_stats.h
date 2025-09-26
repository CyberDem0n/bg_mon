#ifndef _POSTGRES_STATS_H_
#define _POSTGRES_STATS_H_

typedef enum PgBackendType
{
	PG_UNDEFINED = -1,
	PG_UNKNOWN = 0,
	PG_AUTOVAC_LAUNCHER,
	PG_AUTOVAC_WORKER,
	PG_BACKEND,
	PG_DEAD_END_BACKEND,
	PG_BG_WORKER,
	PG_BG_WRITER,
	PG_CHECKPOINTER,
	PG_IO_WORKER,
	PG_STARTUP,
	PG_WAL_RECEIVER,
	PG_WAL_SENDER,
	PG_WAL_WRITER,
	PG_ARCHIVER,
	PG_STATS_COLLECTOR,
	PG_LOGGER,
	PG_STANDALONE_BACKEND,
	PG_SLOTSYNC_WORKER,
	PG_WAL_SUMMARIZER,
	PG_PARALLEL_WORKER,
	PG_LOGICAL_LAUNCHER,
	PG_LOGICAL_TABLESYNC_WORKER,
	PG_LOGICAL_APPLY_WORKER,
	PG_LOGICAL_PARALLEL_WORKER
} PgBackendType;

#define UNKNOWN_NAME "not initialized"
#define AUTOVAC_LAUNCHER_PROC_NAME "autovacuum launcher"
#define AUTOVAC_WORKER_PROC_NAME "autovacuum worker"
#define BACKEND_PROC_NAME "backend"
#define DEAD_END_BACKEND_PROC_NAME "dead-end client backend"
#define STANDALONE_BACKEND_PROC_NAME "standalone backend"
#define SLOTSYNC_WORKER_PROC_NAME "slotsync worker"
#define WAL_SUMMARIZER_PROC_NAME "walsummarizer"
#define BG_WRITER_NAME "bgwriter"
#define CHECKPOINTER_PROC_NAME "checkpointer"
#define IO_WORKER_PROC_NAME "io worker"
#define STARTUP_PROC_NAME "startup"
#define WAL_RECEIVER_NAME "walreceiver"
#define WAL_SENDER_NAME "walsender"
#define WAL_WRITER_NAME "walwriter"
#define ARCHIVER_PROC_NAME "archiver"
#define LOGGER_PROC_NAME "logger"
#define STATS_COLLECTOR_PROC_NAME "stats collector"
#define PARALLEL_WORKER_NAME "parallel worker"
#define LOGICAL_LAUNCHER_NAME "logical replication launcher"
#define LOGICAL_TABLESYNC_WORKER_NAME "logical replication tablesync worker"
#define LOGICAL_APPLY_WORKER_NAME "logical replication apply worker"
#define LOGICAL_PARALLEL_WORKER_NAME "logical replication parallel worker"

typedef struct {
	bool available;
	unsigned long long read_bytes;
	unsigned long read_diff;
	unsigned long long write_bytes;
	unsigned long write_diff;
	unsigned long long cancelled_write_bytes;
	unsigned long cancelled_write_diff;
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
	unsigned long gtime; // (43)
	double gtime_diff;
	unsigned long long uss;
	char *cmdline;
	bool free_cmdline;
	proc_io io;
} proc_stat;

typedef struct {
	pid_t pid;
	Oid	databaseid;
	Oid	userid;
	char *datname;
	char *usename;
	double age;
	double idle_in_transaction_age;
	PgBackendType type;
	BackendState state;
	bool is_blocker;
	uintptr_t blockers;
	uint32 num_blockers;
	pid_t parent_pid;
	uint32 raw_wait_event;
	char *query;
	proc_stat ps;
} pg_stat_activity;

typedef struct {
	bool is_wal_replay_paused;
	TimestampTz last_xact_replay_timestamp;
	XLogRecPtr last_wal_replay_lsn;
	XLogRecPtr current_wal_lsn;
	XLogRecPtr last_wal_receive_lsn;
	int64 current_diff;
	int64 receive_diff;
	int64 replay_diff;
} wal_metrics;

typedef struct {
	pg_stat_activity *values;
	size_t size;
	size_t pos;
	int total_connections;
	int active_connections;
	int idle_in_transaction_connections;
} pg_stat_activity_list;

typedef struct {
	Oid databaseid;
	char *datname;
	int64 n_xact_commit;
	int64 n_xact_commit_diff;
	int64 n_xact_rollback;
	int64 n_xact_rollback_diff;
	int64 n_blocks_fetched;
	int64 n_blocks_fetched_diff;
	int64 n_blocks_hit;
	int64 n_blocks_hit_diff;
	int64 n_tuples_returned;
	int64 n_tuples_returned_diff;
	int64 n_tuples_fetched;
	int64 n_tuples_fetched_diff;
	int64 n_tuples_updated;
	int64 n_tuples_updated_diff;
	int64 n_tuples_inserted;
	int64 n_tuples_inserted_diff;
	int64 n_tuples_deleted;
	int64 n_tuples_deleted_diff;
	int64 n_conflict_tablespace;
	int64 n_conflict_lock;
	int64 n_conflict_snapshot;
	int64 n_conflict_bufferpin;
	int64 n_conflict_startup_deadlock;
	int64 n_temp_files;
	int64 n_temp_files_diff;
	int64 n_temp_bytes;
	int64 n_temp_bytes_diff;
	int64 n_deadlocks;
	int64 n_checksum_failures;
	TimestampTz last_checksum_failure;
	int64 n_block_read_time;       /* times in microseconds */
	double n_block_read_time_diff;
	int64 n_block_write_time;
	double n_block_write_time_diff;
} db_stat;

typedef struct {
	db_stat *values;
	size_t size;
	size_t pos;
	bool track_io_timing;
} db_stat_list;

typedef struct {
	unsigned long long uptime;
	bool recovery_in_progress;
	pg_stat_activity_list activity;
	db_stat_list db;
	wal_metrics wal_metrics;
} pg_stat;

void postgres_stats_init(void);
pg_stat get_postgres_stats(void);

#endif /* _POSTGRES_STATS_H_ */
