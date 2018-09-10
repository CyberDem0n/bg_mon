#!/bin/bash

version=$(postgres -V | sed -n 's/^.* \([1-9][0-9]*\(\.[0-9]*\)\{0,2\}\).*/\1/p')
version=${version%.*}

rm -fr test_cluster*
set -e

readonly port=5440
readonly bport=8090

function shutdown_clusters() {
    set +e
    pg_ctl -w -D test_cluster0 stop -mf
    pg_ctl -w -D test_cluster1 stop -mf
    if [ $version = "10" ]; then
        pg_ctl -w -D test_cluster2 stop -mf
    fi
}

trap shutdown_clusters QUIT TERM INT

function create_cluster() {
    lport=$(($port+$1))
    initdb test_cluster$1
    echo "host replication all 127.0.0.1/32 trust
host replication all ::1/128 trust" >> test_cluster$1/pg_hba.conf
    echo "unix_socket_directories = '.'
hot_standby = 'on'
logging_collector = 'on'
archive_mode = 'on'
archive_command = 'true'
max_wal_senders = 10
wal_keep_segments = 100
shared_preload_libraries = ' bg_mon'
bg_mon.port = $(($bport+$1))" >> test_cluster$1/postgresql.conf
    if [ $version != "9.3" ] && [ $version != "9.4" ]; then
        echo "cluster_name = ' bgworker: \"test cluster$1\" '" >> test_cluster$1/postgresql.conf
    fi
    if [ $version != "9.3" ]; then
        echo "wal_level = 'logical'" >> test_cluster$1/postgresql.conf
    else
        echo "wal_level = 'hot_standby'" >> test_cluster$1/postgresql.conf
    fi
    pg_ctl -w -D test_cluster$1 start -o "--port=$lport"
}

create_cluster 0

echo "create table foo();
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT * FROM foo;
select pg_advisory_lock(1), pg_sleep(30)" | psql -h localhost -p $port -d postgres &
sleep 1
psql -h localhost -p $port -d postgres -c "select pg_advisory_lock(1), pg_sleep(5)" &
sleep 1
echo -ne "SELECT '\"\\\b\013'\f\t\r\n, pg_advisory_lock(1), pg_sleep(5)" | psql -h localhost -p $port -d postgres &

(
    mkdir test_cluster1
    chmod 700 test_cluster1
    if [ $version = "10" ]; then opt="-X none"; fi
    time pg_basebackup $opt -R -c fast -h localhost -p $port -F t -D - | pv -qL 3M | tar -C test_cluster1 -x
    echo "bg_mon.port = $(($bport+1))" >> test_cluster1/postgresql.conf
    pg_ctl -w -D test_cluster1 start -o "--port=$(($port+1))"
    for a in {1..10}; do curl http://localhost:$(($bport+1)) && sleep 1; done
)&

sleep 1

echo "bg_mon.port = $(($bport+3))" >> test_cluster0/postgresql.conf
pg_ctl -D test_cluster0 reload

curl http://localhost:$(($bport+3))/ui > /dev/null
( for a in {1..30}; do curl http://localhost:$(($bport+3)) && echo && sleep 1 && ps auxwwwf | grep postgres; done )&

if [ $version = "10" ]; then
    create_cluster 2

    ( for a in {1..30}; do curl http://localhost:$(($bport+2)) && sleep 1; done )&

    psql -h localhost -p $port -d postgres -c "create table test(id serial not null primary key)"
    psql -h localhost -p $(($port+2)) -d postgres -c "create table test(id serial not null primary key)"
    psql -h localhost -p $port -d postgres -c "insert into test SELECT generate_series(1, 1000000)"
    psql -h localhost -p $port -d postgres -c "CREATE PUBLICATION alltables FOR ALL TABLES"
    psql -h localhost -p $(($port+2)) -d postgres -c "CREATE SUBSCRIPTION mysub CONNECTION 'host=localhost port=$port dbname=postgres' PUBLICATION alltables"
fi

wait

shutdown_clusters
