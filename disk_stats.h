#ifndef _DISK_STATS_H_
#define _DISK_STATS_H_

typedef struct {
	unsigned long long du_data;
	unsigned long du_data_diff;
	unsigned long long du_xlog;
	unsigned long du_xlog_diff;

	unsigned long long data_size;
	unsigned long long data_free;
	unsigned long data_sectors_read;
	unsigned int data_read_diff;
	unsigned long data_sectors_written;
	unsigned int data_write_diff;
	unsigned int data_time_in_queue;
	unsigned int data_time_in_queue_diff;

	unsigned long long xlog_size;
	unsigned long long xlog_free;
	unsigned long xlog_sectors_read;
	unsigned int xlog_read_diff;
	unsigned long xlog_sectors_written;
	unsigned int xlog_write_diff;
	unsigned int xlog_time_in_queue;
	unsigned int xlog_time_in_queue_diff;

	char *data_directory;
	char *data_dev;

	char *xlog_directory;
	char *xlog_dev;
	struct timeval time;
} disk_stat;

void disk_stats_init(void);
disk_stat get_diskspace_stats(void);

#endif /* _DISK_STATS_H_ */
