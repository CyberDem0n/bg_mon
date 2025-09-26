[![Build Status](https://github.com/CyberDem0n/bg_mon/actions/workflows/tests.yaml/badge.svg)](https://github.com/CyberDem0n/bg_mon/actions/workflows/tests.yaml) [![codecov](https://codecov.io/gh/CyberDem0n/bg_mon/graph/badge.svg?token=IS7CWT9FOM)](https://codecov.io/gh/CyberDem0n/bg_mon)

Background worker for monitoring PostgreSQL
===========================================

bg\_mon is a contrib module compatible with PostgreSQL starting from version 9.3
It collects per-process statistics combined with `pg_stat_activity` for the processes that have the rows there, global system stats, per-partition information and the memory stats.

Monitoring is being done from the [Background Worker Process](http://www.postgresql.org/docs/current/bgworker.html) and results are exposed by embeded webserver implemented with the help of [libevent](http://libevent.org/) library.

By default webserver listen on `127.0.0.1:8080` and statistics is collected every second. Optionally, if compiled with libbrotli, the worker will keep statistics in the aggregated format for the past `history_buckets` minutes in memory. Brotli compression is used because the compression ratio is very efficient and most of the modern browsers support it out of the box.

List of available GUC variables
-------------------------------

**bg\_mon.listen\_address** = **'0.0.0.0'** # listen on all available interfaces (default value: 127.0.0.1)

**bg\_mon.port** = **8888** # listen on port 8888 (default value: 8080)

**bg\_mon.naptime** = **10** # collect statistics every 10 seconds (default value: 1)

**bg\_mon.history\_buckets** = **60** # keep one-minute aggregated statistics in memory for 60 minutes (default value: 20)

How to build and install:
-------------------------

1. You have to install `libevent-dev` and optionally `libbrotli-dev` packages
2. $ USE\_PGXS=1 make
3. $ sudo USE\_PGXS=1 make install

How to run it:
--------------
1. Add `shared_preload_libraries = 'bg_mon'` to your postgresql.conf
2. If you want to change default values of `bg_mon.listen_address`, `bg_mon.port`, `bg_mon.naptime`, or `bg_mon.history_buckets` - just add them to the postgresql.conf
3. restart postgresql

If you did everything right, go to you browser and type following URLs to see results:

`http://<bg_mon.listen_address>:<bg_mon.port>/` -- expose collected statistics in a JSON format.

`http://<bg_mon.listen_address>:<bg_mon.port>/X` -- get aggregated statistics from the bucket X, where X is between **0** and **history\_buckets**, **unixtime**, **time**, or **timestamp** in any format supported by postgres (if the timezone is not specified it is assumed to be UTC). The array with the current bucket might be not yet closed.

`http://<bg_mon.listen_address>:<bg_mon.port>/ui` -- simple web page which fetches statistics from server every second and renders it in a simple html format.
