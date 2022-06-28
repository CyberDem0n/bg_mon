#ifndef _DISK_STATS_H_
#define _DISK_STATS_H_

typedef struct device_stat {
	char *name;
	size_t name_len;
	size_t fields;

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
	unsigned long discard_completed;
	unsigned long discard_merges;
	unsigned long discard_sectors;
	unsigned long discard_time;
	unsigned long flush_completed;
	unsigned long flush_time;

	double read_merges_diff;
	double write_merges_diff;
	double discard_merges_diff;
	double read_completed_diff;
	double write_completed_diff;
	double discard_completed_diff;
	double flush_completed_diff;

	double read_diff;
	double write_diff;
	double discard_diff;

	double average_service_time;
	double average_queue_length;
	double average_request_size;
	double read_average_request_size;
	double write_average_request_size;
	double discard_average_request_size;
	double await;
	double read_await;
	double write_await;
	double discard_await;
	double flush_await;

	double util;

	size_t slave_size;
	char slaves[64];
} device_stat;


#define HAS_EXTENDED_STATS(e) (e.fields >= 12)
#define HAS_DISCARD_STATS(e) (e.fields >= 16)
#define HAS_FLUSH_STATS(e) (e.fields >= 18)

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
	disk_stat values[3];
	device_stats dstats;
} disk_stats;

void disk_stats_init(void);
disk_stats get_disk_stats(void);

#endif /* _DISK_STATS_H_ */
