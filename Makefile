# SPDX-License-Identifier: GPL-2.0

CC		= $(CROSS_COMPILE)gcc
SRC_DIR		:= $(CURDIR)
CODE_DIR	:= $(SRC_DIR)/src
APP_DIR		:= $(CODE_DIR)/app
CORE_DIR	:= $(CODE_DIR)/core
PLATFORM_DIR	:= $(CODE_DIR)/platform
OUTPUT_DIR	:= $(CODE_DIR)/output
DOC_DIR		:= $(SRC_DIR)/docs
MANPAGE		:= $(SRC_DIR)/man/armstat.8
ARMSTAT_DATA_DIR = $(DESTDIR)$(PREFIX)/share/armstat
BUILD_OUTPUT	:= $(CURDIR)
PREFIX		?= /usr
DESTDIR		?=
VERSION_FILE	:= $(SRC_DIR)/VERSION
VERSION		:= $(strip $(shell sed -n '1p' "$(VERSION_FILE)" 2>/dev/null))
PROJECT_CPPFLAGS := -I$(APP_DIR) -I$(CORE_DIR) -I$(PLATFORM_DIR) \
		    -I$(OUTPUT_DIR)
COMMON_CFLAGS	:= -Wall -Wextra -Wformat=2 -Wundef -Wshadow \
		   -Wstrict-prototypes -Wmissing-prototypes \
		   -D_FILE_OFFSET_BITS=64 -MMD -MP
DEFAULT_CFLAGS	:= -O2 $(COMMON_CFLAGS) -D_FORTIFY_SOURCE=2
DEFAULT_LDFLAGS	:=
DEBUG_CFLAGS	:= $(COMMON_CFLAGS) -g -O0 \
		   -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS	:= -fsanitize=address,undefined
ANALYZE_CFLAGS	:= -O0 $(COMMON_CFLAGS) -Werror -fanalyzer \
		   -Wno-analyzer-file-leak -Wno-analyzer-malloc-leak
ANALYZE_OUTPUT	= $(BUILD_OUTPUT)/.armstat-analysis

CC_MACHINE := $(shell $(CC) -dumpmachine 2>/dev/null || uname -srm)
CC_VERSION := $(shell $(CC) --version 2>/dev/null | sed -n '1p')

ifeq ($(VERSION),)
$(error VERSION must contain a non-empty version string)
endif

ifneq (,$(findstring linux,$(CC_MACHINE)))
DEFAULT_CFLAGS	+= -fstack-protector-strong -fPIE
DEFAULT_LDFLAGS	+= -pie -Wl,-z,relro,-z,now
endif

CFLAGS ?= $(DEFAULT_CFLAGS)
LDFLAGS ?= $(DEFAULT_LDFLAGS)
VERSION_CPPFLAGS := -DARMSTAT_VERSION=\"$(VERSION)\"

ifeq (command line,$(origin O))
BUILD_OUTPUT	:= $(abspath $(O))
endif

BUILD_CONFIG	:= $(BUILD_OUTPUT)/.armstat-build-config
BUILD_CONFIG_INPUT := CC=$(CC)|CC_MACHINE=$(CC_MACHINE)|CC_VERSION=$(CC_VERSION)|VERSION=$(VERSION)|CPPFLAGS=$(CPPFLAGS)|CFLAGS=$(CFLAGS)|LDFLAGS=$(LDFLAGS)|LDLIBS=$(LDLIBS)
BUILD_CONFIG_KEY := $(shell printf '%s\n' "$(BUILD_CONFIG_INPUT)" | \
	cksum | awk '{print $$1 "-" $$2}')
BUILD_CONFIG_STAMP := $(BUILD_CONFIG).$(BUILD_CONFIG_KEY)

SRCS = app/armstat.c app/armstat_cli.c \
	core/aggregator.c core/collector.c core/cpu_inventory.c \
	core/sample_cache.c core/sampling_deadline.c \
	platform/cpufreq.c platform/cpuidle.c platform/idle_backend.c \
	platform/idle_display.c platform/membw.c platform/pmu.c \
	platform/power.c platform/power_interval.c platform/power_sensor.c \
	platform/sysfs_util.c platform/sysstat.c platform/topology.c \
	output/columns.c output/formatter_csv.c output/formatter_json.c \
	output/formatter_machine.c output/formatter_record.c \
	output/formatter_section.c output/formatter_text.c \
	output/formatter_values.c
OBJ_NAMES = $(SRCS:.c=.o)
OBJ_DIR = $(BUILD_OUTPUT)/.armstat-obj/$(BUILD_CONFIG_KEY)
OBJS = $(addprefix $(OBJ_DIR)/,$(OBJ_NAMES))
TEST_OBJ_NAMES = $(filter-out app/armstat.o,$(OBJ_NAMES))
TEST_OBJS = $(addprefix $(OBJ_DIR)/,$(TEST_OBJ_NAMES))
TARGET = $(BUILD_OUTPUT)/armstat
CONFIG_TARGET = $(BUILD_OUTPUT)/.armstat-bin/$(BUILD_CONFIG_KEY)/armstat
DEP_FILES = $(OBJS:.o=.d)
LEGACY_OBJ_NAMES = $(notdir $(OBJ_NAMES))
LEGACY_OBJS = $(addprefix $(BUILD_OUTPUT)/,$(LEGACY_OBJ_NAMES))
LEGACY_DEP_FILES = $(LEGACY_OBJS:.o=.d)

TEST_NAMES = test_core_logic test_column_selection test_runtime_smoke \
	     test_cpu_inventory test_section_policy
TEST_BINS = $(addprefix $(BUILD_OUTPUT)/tests/,$(TEST_NAMES))
TEST_WRAPPERS = $(addprefix tests/,$(TEST_NAMES))
TEST_DEP_FILES = $(TEST_BINS:%=%.d)

.DEFAULT_GOAL := all

.PHONY: all armstat FORCE
all: $(TARGET)

FORCE:

$(BUILD_CONFIG_STAMP):
	@mkdir -p $(dir $@)
	@printf '%s\n' "CC=$(CC)" "CC_MACHINE=$(CC_MACHINE)" \
		"CC_VERSION=$(CC_VERSION)" "VERSION=$(VERSION)" \
		"CPPFLAGS=$(CPPFLAGS)" "CFLAGS=$(CFLAGS)" \
		"LDFLAGS=$(LDFLAGS)" "LDLIBS=$(LDLIBS)" > "$@.tmp"
	@mv "$@.tmp" "$(BUILD_CONFIG)"
	@touch "$@"

# Compatibility wrapper for callers that explicitly use `make armstat`.
armstat: $(TARGET)

$(CONFIG_TARGET): $(BUILD_CONFIG_STAMP) $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

$(TARGET): $(CONFIG_TARGET) FORCE
	@if ! cmp -s "$(CONFIG_TARGET)" "$@"; then \
		cp "$(CONFIG_TARGET)" "$@.$(BUILD_CONFIG_KEY).tmp"; \
		mv "$@.$(BUILD_CONFIG_KEY).tmp" "$@"; \
	fi

$(OBJ_DIR)/%.o: $(CODE_DIR)/%.c $(BUILD_CONFIG_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(VERSION_CPPFLAGS) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

-include $(DEP_FILES) $(TEST_DEP_FILES)

.PHONY: clean
clean:
	@rm -f $(TARGET) $(OBJS) $(DEP_FILES) $(TEST_BINS) $(TEST_DEP_FILES) \
		$(LEGACY_OBJS) $(LEGACY_DEP_FILES) \
		$(BUILD_CONFIG) $(BUILD_CONFIG).tmp $(BUILD_CONFIG).* \
		$(TARGET).*.tmp \
		$(BUILD_OUTPUT)/*.gcda $(BUILD_OUTPUT)/*.gcno $(BUILD_OUTPUT)/*.gcov \
		$(BUILD_OUTPUT)/tests/*.gcda $(BUILD_OUTPUT)/tests/*.gcno \
		$(BUILD_OUTPUT)/tests/*.gcov
	@rm -rf $(TARGET).dSYM $(addsuffix .dSYM,$(TEST_BINS))
	@rm -rf $(BUILD_OUTPUT)/.armstat-obj $(BUILD_OUTPUT)/.armstat-bin
	@rm -rf $(ANALYZE_OUTPUT)
	@rm -rf $(SRC_DIR)/scripts/__pycache__ $(SRC_DIR)/tests/__pycache__

.PHONY: debug
debug:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(DEBUG_CFLAGS)" LDFLAGS="$(DEBUG_LDFLAGS)" armstat

.PHONY: debug-test
debug-test:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(DEBUG_CFLAGS)" LDFLAGS="$(DEBUG_LDFLAGS)" test

.PHONY: analyze
analyze:
	$(MAKE) O="$(ANALYZE_OUTPUT)" CFLAGS="$(ANALYZE_CFLAGS)" \
		LDFLAGS= armstat

.PHONY: install
install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/armstat
	install -m 755 scripts/plot_sum.py \
		$(DESTDIR)$(PREFIX)/bin/armstat-plot-summary
	install -m 755 scripts/plot_cpu.py \
		$(DESTDIR)$(PREFIX)/bin/armstat-plot-cpu
	install -d $(ARMSTAT_DATA_DIR)
	install -m 644 scripts/plot_utils.py scripts/armstat_loader.py \
		$(ARMSTAT_DATA_DIR)
	install -d $(DESTDIR)$(PREFIX)/share/man/man8
	install -m 644 $(MANPAGE) $(DESTDIR)$(PREFIX)/share/man/man8/armstat.8
	install -d $(DESTDIR)$(PREFIX)/share/doc/armstat
	install -m 644 COPYING VERSION README.md README.zh-CN.md \
		$(DESTDIR)$(PREFIX)/share/doc/armstat
	install -d $(DESTDIR)$(PREFIX)/share/doc/armstat/docs
	install -m 644 $(DOC_DIR)/REFERENCE.md $(DOC_DIR)/REFERENCE.zh-CN.md \
		$(DESTDIR)$(PREFIX)/share/doc/armstat/docs

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/armstat
	rm -f $(DESTDIR)$(PREFIX)/bin/armstat-plot-summary
	rm -f $(DESTDIR)$(PREFIX)/bin/armstat-plot-cpu
	rm -rf $(ARMSTAT_DATA_DIR)
	rm -f $(DESTDIR)$(PREFIX)/share/man/man8/armstat.8
	rm -rf $(DESTDIR)$(PREFIX)/share/doc/armstat

.PHONY: test
test: $(TARGET) $(TEST_BINS)
	$(BUILD_OUTPUT)/tests/test_core_logic
	$(BUILD_OUTPUT)/tests/test_column_selection
	$(BUILD_OUTPUT)/tests/test_runtime_smoke
	$(BUILD_OUTPUT)/tests/test_cpu_inventory
	$(BUILD_OUTPUT)/tests/test_section_policy
	ARMSTAT_BIN=$(TARGET) sh $(SRC_DIR)/tests/test_cli_smoke.sh
	python3 $(SRC_DIR)/tests/test_plot_loaders.py
	python3 $(SRC_DIR)/tests/test_plot_render.py
	python3 $(SRC_DIR)/tests/test_csv_streaming.py
	sh $(SRC_DIR)/tests/test_build.sh

.PHONY: target-test
target-test: $(TARGET)
	ARMSTAT_BIN=$(TARGET) sh $(SRC_DIR)/tests/test_target_arm64.sh

# Test executables have stable public paths, so force their final link step.
# Their object prerequisites are configuration-keyed, but without this guard a
# newer executable from another CFLAGS/LDFLAGS configuration could be reused.
$(BUILD_OUTPUT)/tests/%: $(SRC_DIR)/tests/%.c $(TEST_OBJS) FORCE
	@mkdir -p $(dir $@)
	$(CC) $(VERSION_CPPFLAGS) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CFLAGS) \
		$< $(TEST_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

.PHONY: $(TEST_WRAPPERS)
$(TEST_WRAPPERS): tests/%: $(BUILD_OUTPUT)/tests/%
