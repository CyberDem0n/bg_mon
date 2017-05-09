Background worker for monitoring PostgreSQL
===========================================

bg_mon is an extension compatible with PostgreSQL starting from version 9.3
It collects per-process statistics combined with `pg_stat_activity` for the processes that have the rows there, global system stats, per-partition information and the memory stats.

Monitoring is being done from the [Background Worker Process](http://www.postgresql.org/docs/9.3/static/bgworker.html) and results are exposed by embeded webserver implemented with the help of [libevent](http://libevent.org/) library.

By default webserver listen on `127.0.0.1:8080` and statistics is collected every second.
These values can be overriden with followin GUC variables:

**bg_mon.listen_address** = **'0.0.0.0'** # listen on all available interfaces (default value: 127.0.0.1)

**bg_mon.port** = **8888** # listen on port 8888 (default value: 8080)

**bg_mon.naptime** = **10** # collect statistics every 10 seconds (default value: 1)

How to build and install:
-------------------------

1. You have to install `libevent-dev` package
2. $ USE_PGXS=1 make
3. $ sudo USE_PGXS=1 make install

How to run it:
--------------
1. Add `shared_preload_libraries = 'bg_mon'` to your postgresql.conf
2. If you want to change default values of `bg_mon.listen_address`, `bg_mon.port` or `bg_mon.naptime` - just add them to the postgresql.conf
3. restart postgresql

If you did everything right, go to you browser and type following URLs to see results:

`http://<bg_mon.listen_address>:<bg_mon.port>/` -- expose collected statistics in a JSON format.

`http://<bg_mon.listen_address>:<bg_mon.port>/ui` -- simple web page which fetches statistics from server every second and renders it in a simple html format.


License
-------
[Apache 2.0](http://www.apache.org/licenses/LICENSE-2.0)
