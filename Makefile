# SPDX-License-Identifier: GPL-2.0
CC		= $(CROSS_COMPILE)gcc
BUILD_OUTPUT	:= $(CURDIR)
PREFIX		?= /usr
DESTDIR		?=
DAY		:= $(shell date +%Y.%m.%d)
SNAPSHOT	= armstat-$(DAY)
COMMON_CFLAGS	:= -Wall -Wextra -I../../../include \
		   -D_FILE_OFFSET_BITS=64 -MMD -MP
DEFAULT_CFLAGS	:= -O2 $(COMMON_CFLAGS) -D_FORTIFY_SOURCE=2
DEBUG_CFLAGS	:= $(COMMON_CFLAGS) -g -O0 \
		   -fsanitize=address,undefined -fno-omit-frame-pointer

ifeq ("$(origin O)", "command line")
	BUILD_OUTPUT := $(O)
endif

SRCS = armstat.c armstat_cli.c cpufreq.c cpuidle.c power.c pmu.c topology.c sysstat.c \
       collector.c cpu_inventory.c sample_cache.c \
       idle_backend.c aggregator.c formatter.c formatter_record.c \
       formatter_text.c formatter_machine.c power_sensor.c power_interval.c membw.c
OBJS = $(SRCS:.c=.o)
TEST_OBJS = $(filter-out armstat.o,$(OBJS))

-include $(BUILD_OUTPUT)/*.d

armstat : $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(BUILD_OUTPUT)/armstat $(LDFLAGS)

CFLAGS ?= $(DEFAULT_CFLAGS)

%: %.c
	@mkdir -p $(BUILD_OUTPUT)
	$(CC) $(CFLAGS) $< -o $(BUILD_OUTPUT)/$@ $(LDFLAGS)

.PHONY : clean
clean :
	@rm -f $(BUILD_OUTPUT)/armstat
	@rm -f $(OBJS)
	@rm -f *.o
	@rm -f $(BUILD_OUTPUT)/*.d
	@rm -f $(SNAPSHOT).tar.gz
	@rm -rf scripts/__pycache__
	@rm -f tests/test_core_logic
	@rm -f tests/test_column_selection
	@rm -f tests/test_runtime_smoke

.PHONY : debug
debug :
	$(MAKE) clean
	$(MAKE) CFLAGS="$(DEBUG_CFLAGS)" armstat

install : armstat
	install -d $(DESTDIR)$(PREFIX)/bin
	install $(BUILD_OUTPUT)/armstat $(DESTDIR)$(PREFIX)/bin/armstat
	install -d $(DESTDIR)$(PREFIX)/share/man/man8
	install -m 644 armstat.8 $(DESTDIR)$(PREFIX)/share/man/man8
	install -d $(DESTDIR)$(PREFIX)/share/doc/armstat
	install -m 644 README.md README.zh-CN.md DESIGN.md DESIGN.zh-CN.md \
		EXPORTS.md EXPORTS.zh-CN.md \
		PLOTTING.md PLOTTING.zh-CN.md TESTING.md TESTING.zh-CN.md \
		$(DESTDIR)$(PREFIX)/share/doc/armstat
	install -d $(DESTDIR)$(PREFIX)/share/doc/armstat/scripts
	install -m 755 scripts/plot_sum.py scripts/plot_cpu.py scripts/plot_utils.py \
		$(DESTDIR)$(PREFIX)/share/doc/armstat/scripts

.PHONY : test
test : armstat tests/test_core_logic tests/test_column_selection tests/test_runtime_smoke
	./tests/test_core_logic
	./tests/test_column_selection
	./tests/test_runtime_smoke
	sh ./tests/test_cli_smoke.sh
	python3 tests/test_plot_loaders.py

tests/test_core_logic : tests/test_core_logic.c $(TEST_OBJS)
	$(CC) $(CFLAGS) tests/test_core_logic.c $(TEST_OBJS) -o $@ $(LDFLAGS)

tests/test_column_selection : tests/test_column_selection.c $(TEST_OBJS)
	$(CC) $(CFLAGS) tests/test_column_selection.c $(TEST_OBJS) -o $@ $(LDFLAGS)

tests/test_runtime_smoke : tests/test_runtime_smoke.c $(TEST_OBJS)
	$(CC) $(CFLAGS) tests/test_runtime_smoke.c $(TEST_OBJS) -o $@ $(LDFLAGS)
