#!/bin/bash

version=$(postgres -V | sed -n 's/^.* \([1-9][0-9]*\(\.[0-9]*\)\{0,2\}\).*/\1/p')
version=${version%.*}

rm -fr test_cluster*
set -e

readonly port=5440
readonly bport=8090
background_pids=()

function run_bg() {
    cmd=$1; shift
    "$cmd" "$@" &
    background_pids+=($!)
}

function run_sql_bg() {
    echo -ne "$1" | psql -h localhost -p $port -d postgres &
    background_pids+=($!)
}

function shutdown_clusters() {
    set +e
    pg_ctl -w -D test_cluster0 stop -mf
    pg_ctl -w -D test_cluster1 stop -mf
    if [[ $version =~ ^[1-9][0-9]$ ]]; then
        pg_ctl -w -D test_cluster2 stop -mf
    fi
}

trap shutdown_clusters QUIT TERM INT

function start_postgres() {
    postgres -D test_cluster$1 --port=$(($port+$1)) &
    max_attempts=0
    while ! pg_isready -h localhost -p $(($port+$1)) -d postgres; do
        [[ $((max_attempts++)) -lt 10 ]] && sleep 1 || exit 1
    done

    if [ -x ./noisia ]; then
        if [ "$1" = "1" ]; then
            OPTS=""
        else
            OPTS="--deadlocks --wait-xacts --temp-files --rollbacks"
            psql -h localhost -c 'CREATE USER noisia' -p $(($port+$1)) postgres
        fi
        ./noisia --jobs=5 --duration=30 --idle-xacts $OPTS --conninfo="host=localhost port=$(($port+$1)) user=noisia dbname=postgres application_name=noisia" > noisia.$1.log 2>&1 &
    fi
}

function create_cluster() {
    initdb -k test_cluster$1
    echo "host replication all 127.0.0.1/32 trust
host replication all ::1/128 trust" >> test_cluster$1/pg_hba.conf
    echo "unix_socket_directories = '.'
hot_standby = 'on'
logging_collector = 'on'
log_directory = 'pg_log'
log_line_prefix = '%u %m [%p] '
log_filename = 'postgresql.log'
track_io_timing = 'on'
archive_mode = 'on'
archive_command = 'true'
max_wal_senders = 10
shared_preload_libraries = 'bg_mon'
bg_mon.port = $(($bport+$1))" >> test_cluster$1/postgresql.conf
    if [ $version != "9.3" ] && [ $version != "9.4" ]; then
        echo "cluster_name = ' bgworker: \"test cluster$1\" '" >> test_cluster$1/postgresql.conf
    fi
    if [ $version != "9.3" ]; then
        echo "wal_level = 'logical'" >> test_cluster$1/postgresql.conf
    else
        echo "wal_level = 'hot_standby'" >> test_cluster$1/postgresql.conf
    fi
    if [[ ${version%.*} -ge 17 ]]; then
        echo "sync_replication_slots = 'on'
hot_standby_feedback = 'on'
primary_slot_name = 'standby'
summarize_wal = 'on'" >> test_cluster$1/postgresql.conf
    fi
    start_postgres $1
}

function curl_ps_loop() {
    for a in $(seq 1 $2); do
        curl -s -H "Accept-Encoding: br" http://localhost:$(($bport+$1)) | brotli -d || true
        sleep 1
        if [[ ! -z "$3" ]]; then
            ps auxwwwf | grep postgres
        fi
    done
    curl -s http://localhost:$(($bport+$1))
    for a in $(seq -f '%02g' 0 19); do
        curl -s http://localhost:$(($bport+$1))/$a > /dev/null
    done
    curl -s http://localhost:$(($bport+$1))/prev > /dev/null
    curl -s http://localhost:$(($bport+$1))/$(date +'%H:%M' -ud '2 hour') > /dev/null
    curl -s http://localhost:$(($bport+$1))/$(date +'%H:%M' -ud '1 minute ago') > /dev/null
    curl -s http://localhost:$(($bport+$1))/$(date +'%s' -ud '1 minute ago') > /dev/null
    curl -s http://localhost:$(($bport+$1))/$(date +'%s' -ud '1 hour ago') > /dev/null
    curl -s http://localhost:$(($bport+$1))/$(date | sed 's/ /+/g') > /dev/null
    curl -s http://localhost:$(($bport+$1))/now > /dev/null
    curl -s http://localhost:$(($bport+$1))/blablabla > /dev/null
    curl -s http://localhost:$(($bport+$1))/100000000000000000000000000000000000000000000 > /dev/null
}

function clone_cluster() {
    mkdir test_cluster$1
    chmod 700 test_cluster1
    if [[ $version =~ ^[1-9][0-9]$ ]]; then opt="-X none"; fi
    time pg_basebackup $opt --dbname "host=localhost port=$port dbname=postgres" -R -c fast -F t -D - | pv -qL 3M | tar -C test_cluster1 -x || true
    echo "bg_mon.port = $(($bport+$1))" >> test_cluster$1/postgresql.conf
    start_postgres $1
    curl_ps_loop $1 10
}

create_cluster 0

if [[ ${version%.*} -ge 17 ]]; then
    psql -h localhost -p $port -d postgres -c "select pg_create_physical_replication_slot('standby')"
    psql -h localhost -p $port -d postgres -c "select pg_create_logical_replication_slot('failover', 'pgoutput', failover => true)"
fi

if [[ ${version%.*} -ge 13 ]]; then
    # PROCSIG_BARRIER handling test
    psql -h localhost -p $port -d postgres -c "CREATE DATABASE test"
    sleep 1
    psql -h localhost -p $port -d test -c "SELECT pg_sleep(10)" &
    procsig_pid=($!)
    sleep 1
    psql -h localhost -p $port -d postgres -c "DROP DATABASE test WITH (FORCE)"
    sleep 1
    ps -p $procsig_pid && exit 1
fi

run_sql_bg "create table foo(id int not null primary key); INSERT INTO foo values(1); BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE; SELECT * FROM foo WHERE id = 1; select pg_advisory_lock(1), pg_sleep(30);"
sleep 1
run_sql_bg "select pg_advisory_lock(1), pg_sleep(5)"
sleep 1
run_sql_bg "SELECT '\"\\\b\013'\f\t\r\n, pg_advisory_lock(1), pg_sleep(5)"

run_bg clone_cluster 1

sleep 1

echo "bg_mon.port = $(($bport+3))" >> test_cluster0/postgresql.conf
pg_ctl -D test_cluster0 reload

max_attempts=0
while ! curl http://localhost:$(($bport+3))/ui > /dev/null; do
    [[ $((max_attempts++)) -lt 5 ]] && sleep 1 || exit 1
done

run_bg curl_ps_loop 3 30 1

if [[ $version =~ ^[1-9][0-9]$ ]]; then
    create_cluster 2
    run_bg curl_ps_loop 2 30

    psql -h localhost -p $port -d postgres -c "create table test(id serial not null primary key)"
    psql -h localhost -p $(($port+2)) -d postgres -c "create table test(id serial not null primary key)"
    psql -h localhost -p $port -d postgres -c "insert into test SELECT generate_series(1, 1000000)"
    psql -h localhost -p $port -d postgres -c "CREATE PUBLICATION test FOR TABLE test"
    psql -h localhost -p $(($port+2)) -d postgres -c "CREATE SUBSCRIPTION mysub CONNECTION 'host=localhost port=$port dbname=postgres' PUBLICATION test"
fi

wait ${background_pids[@]}

shutdown_clusters
