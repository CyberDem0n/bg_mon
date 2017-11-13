MODULE_big = bg_mon
OBJS = bg_mon.o postgres_stats.o disk_stats.o system_stats.o
UIFILENAME = bg_mon.html
PG_CPPFLAGS = -DUIFILE='"$(DESTDIR)$(datadir)/$(datamoduledir)/$(UIFILENAME)"'
ifdef ENABLE_GCOV
PG_CPPFLAGS += -g -ggdb -pg -O0 -fprofile-arcs -ftest-coverage
endif
DATA = $(UIFILENAME)
PGFILEDESC = 'Background worker for monitoring postgresql instance from inside'

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

SHLIB_LINK += -levent -levent_pthreads -pthread
ifdef ENABLE_GCOV
SHLIB_LINK += -lgcov --coverage
endif
