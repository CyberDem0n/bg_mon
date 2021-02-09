#ifndef _POSTGRES_STATS_H_
#define _POSTGRES_STATS_H_

typedef enum PgBackendType
{
	PG_UNDEFINED = -1,
	PG_UNKNOWN = 0,
	PG_AUTOVAC_LAUNCHER,
	PG_AUTOVAC_WORKER,
	PG_BACKEND,
	PG_BG_WORKER,
	PG_BG_WRITER,
	PG_CHECKPOINTER,
	PG_STARTUP,
	PG_WAL_RECEIVER,
	PG_WAL_SENDER,
	PG_WAL_WRITER,
	PG_ARCHIVER,
	PG_STATS_COLLECTOR,
	PG_LOGGER,
	PG_PARALLEL_WORKER,
	PG_LOGICAL_LAUNCHER,
	PG_LOGICAL_WORKER
} PgBackendType;

#define UNKNOWN_NAME "not initialized"
#define AUTOVAC_LAUNCHER_PROC_NAME "autovacuum launcher"
#define AUTOVAC_WORKER_PROC_NAME "autovacuum worker"
#define BACKEND_PROC_NAME "backend"
#define BG_WRITER_NAME "bgwriter"
#define CHECKPOINTER_PROC_NAME "checkpointer"
#define STARTUP_PROC_NAME "startup"
#define WAL_RECEIVER_NAME "walreceiver"
#define WAL_SENDER_NAME "walsender"
#define WAL_WRITER_NAME "walwriter"
#define ARCHIVER_PROC_NAME "archiver"
#define LOGGER_PROC_NAME "logger"
#define STATS_COLLECTOR_PROC_NAME "stats collector"
#define PARALLEL_WORKER_NAME "parallel worker"
#define LOGICAL_LAUNCHER_NAME "logical replication launcher"
#define LOGICAL_WORKER_NAME "logical replication worker"

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
} pg_stat;

typedef struct {
        bool is_wal_replay_paused;
        TimestampTz last_xact_replay_timestamp; 
        XLogRecPtr last_wal_replay_lsn;
        XLogRecPtr current_wal_lsn;
        XLogRecPtr last_wal_receive_lsn;
} wal_metrics;

typedef struct {
	pg_stat *values;
	size_t size;
	size_t pos;
	unsigned long long uptime;
	bool recovery_in_progress;
	int total_connections;
	int active_connections;
	int idle_in_transaction_connections;
        wal_metrics wal;
} pg_stat_list;

void postgres_stats_init(void);
pg_stat_list get_postgres_stats(void);

#endif /* _POSTGRES_STATS_H_ */
