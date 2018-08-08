#ifndef _NET_STATS_H_
#define _NET_STATS_H_
#include <net/if.h>

#include "postgres.h"

typedef struct net_stat {
	char name[IF_NAMESIZE + 1];
	unsigned long long rx_bytes;
	unsigned long long rx_packets;
	unsigned long long rx_errors;
	unsigned long long tx_bytes;
	unsigned long long tx_packets;
	unsigned long long tx_errors;
	unsigned long long collisions;
	unsigned long long saturation;

	long long speed;
	char duplex;

	double rx_bytes_diff;
	double rx_packets_diff;
	double rx_errors_diff;
	double rx_util;
	double tx_bytes_diff;
	double tx_packets_diff;
	double tx_errors_diff;
	double tx_util;

	double util;
	double collisions_diff;
	double saturation_diff;


	bool is_used;
	bool has_statistics;
} net_stat;

typedef struct net_stats {
	unsigned long long uptime;
	net_stat *values;
	size_t size;
	size_t len;
} net_stats;

void net_stats_init(void);
net_stats get_net_stats(void);

#endif /* _NET_STATS_H_ */
