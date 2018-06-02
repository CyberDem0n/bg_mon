#ifndef _POSTGRES_STATS_H_
#define _POSTGRES_STATS_H_

typedef enum PgBackendType
{
	PG_UNDEFINED = -2,
	PG_UNKNOWN = -1,
	PG_AUTOVAC_LAUNCHER = 0,
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
	PG_LOGGER,
	PG_STATS_COLLECTOR,
	PG_LOGICAL_LAUNCHER,
	PG_LOGICAL_WORKER
} PgBackendType;

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
	char *datname;
	char *usename;
	int32 age;
	PgBackendType type;
	char *locked_by;
	char *query;
	proc_stat ps;
} pg_stat;

typedef struct {
	pg_stat *values;
	size_t size;
	size_t pos;
	unsigned long long uptime;
	bool recovery_in_progress;
	int total_connections;
	int active_connections;
} pg_stat_list;

void postgres_stats_init(void);
pg_stat_list get_postgres_stats(void);

#endif /* _POSTGRES_STATS_H_ */
