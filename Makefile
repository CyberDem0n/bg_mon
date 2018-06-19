MODULE_big = bg_mon
OBJS = $(patsubst %.c,%.o,$(wildcard *.c))
UIFILENAME = bg_mon.html
PG_CPPFLAGS = -DUIFILE='"$(DESTDIR)$(datadir)/$(datamoduledir)/$(UIFILENAME)"'
HAS_LIBBROTLI := $(shell ldconfig -p 2> /dev/null | grep -q libbrotlienc; echo $$?)
ifeq ($(HAS_LIBBROTLI),0)
    PG_CPPFLAGS += -DHAS_LIBBROTLI
else
    OBJS := $(filter-out brotli_utils.o,$(OBJS))
endif
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
ifeq ($(HAS_LIBBROTLI),0)
    SHLIB_LINK += -lbrotlienc
endif
ifdef ENABLE_GCOV
    SHLIB_LINK += -lgcov --coverage
endif
