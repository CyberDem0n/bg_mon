#ifndef _DISK_STATS_H_
#define _DISK_STATS_H_

typedef struct device_stat {
	char *name;
	size_t name_len;
	bool is_used;
	unsigned long read_completed;
	unsigned long read_merges;
	unsigned long read_sectors;
	unsigned long write_completed;
	unsigned long write_merges;
	unsigned long write_sectors;
	unsigned int read_time;
	unsigned int write_time;
	unsigned int ios_in_progress;
	unsigned int total_time;
	unsigned int weighted_time;

	double read_merges_diff;
	double write_merges_diff;
	double read_completed_diff;
	double write_completed_diff;

	double read_diff;
	double write_diff;

	double average_service_time;
	double average_queue_length;
	double average_request_size;
	double await;
	double read_await;
	double write_await;

	double util;

	bool extended;

	size_t slave_size;
	char slaves[64];
} device_stat;

typedef struct {
	unsigned long long uptime;
	device_stat *values;
	unsigned char size;
	unsigned int len;
} device_stats;

typedef struct disk_stat {
	unsigned long long du;
	unsigned long long size;
	unsigned long long free;
	char *type;
	char *directory;
	int device_id;
} disk_stat;

typedef struct {
	disk_stat values[2];
	device_stats dstats;
} disk_stats;

void disk_stats_init(void);
disk_stats get_disk_stats(void);

#endif /* _DISK_STATS_H_ */
