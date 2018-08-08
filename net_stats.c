#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <linux/types.h>
#include <linux/ethtool.h>

#include "system_stats.h"
#include "net_stats.h"

#ifndef	DUPLEX_UNKNOWN
#define	DUPLEX_UNKNOWN		0xFF
#endif /* DUPLEX_UNKNOWN */

extern system_stat system_stats_old;

static net_stats net_stats_old = {0,};
static net_stats net_stats_next = {0,};

static void read_net_stats(net_stats *ns)
{
	char *p;
	char buf[256], name[IF_NAMESIZE + 1];
	unsigned long long rx_bytes, rx_packets, rx_errors, tx_bytes, tx_packets, tx_errors, collisions, sat[4];
	int n, j = 0;
	net_stat *stats = ns->values;
	FILE *net = fopen("/proc/net/dev", "r");

	if (net == NULL) return;

	while (fgets(buf, sizeof(buf), net)) {
		if (j++ < 2) continue; /* skip headers */
		if (sscanf(buf, "%s %llu %llu %llu %llu %*u %*u %*u %*u %llu %llu %llu %llu %llu %llu %llu %*u",
					name, &rx_bytes, &rx_packets, &rx_errors, &sat[0], &tx_bytes, &tx_packets,
					&tx_errors, &sat[1], &sat[2], &collisions, &sat[3]) == 12) {
			/* Skip interface if it has never seen a packet */
			if (rx_packets == 0 && tx_packets == 0) continue;

			if ((p = strrchr(name, ':')))
				*p = '\0';

			for (n = 0; n < ns->size; ++n)
				if (strcmp(stats[n].name, name) == 0)
					break;

			if (n == ns->size) { /* interface is not in our list */
				int sock;
				if (ns->size++ >= ns->len)
					stats = ns->values = repalloc(ns->values, sizeof(net_stat)*(ns->len = ns->size));

				memset(stats + n, 0, sizeof(net_stat));
				strcpy(stats[n].name, name);
				stats[n].speed = -1;
				stats[n].duplex = DUPLEX_UNKNOWN;

				if ((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP)) >= 0) {
					struct ifreq ifr;
					struct ethtool_cmd edata;
					strcpy(ifr.ifr_name, name);
					ifr.ifr_data = (void *) &edata;
					edata.cmd = ETHTOOL_GSET;
					if (ioctl(sock, SIOCETHTOOL, &ifr) >= 0) {
						stats[n].speed = (long long) edata.speed * 1000000;
						stats[n].duplex = edata.duplex;
					}
					close(sock);
				}
			}

			stats[n].rx_bytes = rx_bytes;
			stats[n].rx_packets = rx_packets;
			stats[n].rx_errors = rx_errors;
			stats[n].tx_bytes = tx_bytes;
			stats[n].tx_packets = tx_packets;
			stats[n].tx_errors = tx_errors;
			stats[n].collisions = collisions;
			stats[n].saturation = rx_errors + collisions + sat[0] + sat[1] + sat[2] + sat[3];
			stats[n].is_used = true;
		}
	}
	fclose(net);
}

static void copy_net_stats(net_stats o, net_stats *n)
{
	int i, len = 0;
	for (i = 0; i < o.size; ++i)
		if (o.values[i].is_used)
			++len;
	if (len == 0) return;
	else if (len > n->len)
		n->values = repalloc(n->values, (n->len = len)*sizeof(net_stat));

	n->size = 0;

	for (i = 0; i < o.size; ++i)
		if (o.values[i].is_used) {
			memset(n->values + n->size, 0, sizeof(net_stat));
			strcpy(n->values[n->size].name, o.values[i].name);
			n->values[n->size].speed = o.values[i].speed;
			n->values[n->size++].duplex = o.values[i].duplex;
		}
}

static void diff_net_stats(net_stats *new_stats)
{
	int i, j = 0;
	net_stat o;
	unsigned long long itv;

	new_stats->uptime = system_stats_old.uptime;

	if (net_stats_old.uptime == 0) return;

	itv = new_stats->uptime - net_stats_old.uptime;

	for (i = 0; i < new_stats->size; ++i) {
		net_stat *n = new_stats->values + i;

		while (j < net_stats_old.size && !net_stats_old.values[j].is_used)
			++j;

		if (j >= net_stats_old.size)
			return;

		o = net_stats_old.values[j++];

		n->has_statistics = true;
		n->rx_bytes_diff = S_VALUE(o.rx_bytes, n->rx_bytes, itv);
		n->rx_packets_diff = S_VALUE(o.rx_packets, n->rx_packets, itv);
		n->rx_errors_diff = S_VALUE(o.rx_errors, n->rx_errors, itv);
		n->tx_bytes_diff = S_VALUE(o.tx_bytes, n->tx_bytes, itv);
		n->tx_packets_diff = S_VALUE(o.tx_packets, n->tx_packets, itv);
		n->tx_errors_diff = S_VALUE(o.tx_errors, n->tx_errors, itv);
		n->collisions_diff = S_VALUE(o.collisions, n->collisions, itv);
		n->saturation_diff = S_VALUE(o.saturation, n->saturation, itv);

		if (n->speed > 0) {
			n->rx_util = MINIMUM(n->rx_bytes_diff * 800.0 / n->speed, 100);
			n->tx_util = MINIMUM(n->tx_bytes_diff * 800.0 / n->speed, 100);
			if (n->duplex == 1) /* full duplex */
				n->util = MAXIMUM(n->rx_util, n->tx_util);
			else if (n->duplex == 0) /* half duplex */
				n->util = MINIMUM((n->rx_bytes_diff + n->tx_bytes_diff)*800.0/n->speed, 100);
		} else n->rx_util = n->tx_util = n->util = 0;
	}
}

net_stats get_net_stats(void)
{
	net_stats ret = net_stats_next;
	copy_net_stats(net_stats_old, &ret);

	read_net_stats(&ret);

	diff_net_stats(&ret);

	net_stats_next = net_stats_old;
	return net_stats_old = ret;
}

void net_stats_init(void)
{
	net_stats_old.values = palloc(sizeof(net_stat));
	net_stats_next.values = palloc(sizeof(net_stat));
	net_stats_old.len = net_stats_next.len = 1;
	net_stats_old.size = net_stats_next.size = 0;
	net_stats_old.uptime = net_stats_next.uptime = 0;
}
