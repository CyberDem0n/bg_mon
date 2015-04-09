MODULE_big = bg_mon
OBJS = bg_mon.o postgres_stats.o disk_stats.o system_stats.o

EXTENSION = bg_mon
DATA = bg_mon--0.1.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/bg_mon
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += -levent -pthread
