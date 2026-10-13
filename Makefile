CC ?= cc
AR ?= ar
NINJA ?= ninja
CLANG ?= clang
CLANG_TIDY ?= clang-tidy
GCC_ANALYZER ?= gcc
CPPCHECK ?= cppcheck
CPPCHECK_FLAGS ?= --enable=warning,performance,portability --check-level=exhaustive --force
PREFIX ?= /usr/local
DESTDIR ?=
ARCH ?= $(shell uname -m)
PACKAGE_VERSION ?= 1.0.0
SCDOC ?= scdoc
SANITIZERS ?= address,undefined
SANITIZER_CFLAGS ?= -fsanitize=$(SANITIZERS) -fno-omit-frame-pointer
SANITIZER_LDFLAGS ?= -fsanitize=$(SANITIZERS)
SANITIZE_BUILD_DIR ?= build-sanitize

BUILD_DIR := build
DOC_DIR := docs
DOC_BUILD_DIR := $(BUILD_DIR)/docs
DOC_FILES := $(wildcard $(DOC_DIR)/*.md)
DOC_BASENAMES := $(notdir $(DOC_FILES))
MAN_SOURCE := $(DOC_DIR)/quicksand.3.scd
MAN_PAGE := $(DOC_BUILD_DIR)/quicksand.3

LIB_SHARED := $(BUILD_DIR)/libquicksand.so
LIB_STATIC := $(BUILD_DIR)/libquicksand.a
PC_FILE := $(BUILD_DIR)/quicksand.pc

CPPFLAGS ?= -Iinclude
CFLAGS ?= -O3 -g -Wall -Wextra -std=c11 -fPIC
LDFLAGS ?=
LDLIBS ?= -lm

ifeq ($(ARCH),arm64)
ARCH := aarch64
endif

ifeq ($(ARCH),x86_64)
	CFLAGS += -march=native -mtune=native
endif

ifeq ($(ARCH),aarch64)
	CFLAGS += -mcpu=native
endif

TIMESTAMP_SRC := src/timestamp+$(ARCH).s

ifeq ($(wildcard $(TIMESTAMP_SRC)),)
$(error Unsupported ARCH "$(ARCH)" (missing $(TIMESTAMP_SRC)))
endif

LIB_OBJECTS := $(BUILD_DIR)/quicksand.o $(BUILD_DIR)/time.o $(BUILD_DIR)/timestamp.o

TEST_SOURCES := test/test_basic.c test/test_time.c test/test_pub.c test/test_sub.c
TEST_BINS := $(patsubst test/test_%.c,$(BUILD_DIR)/test/%,$(TEST_SOURCES))
ANALYZE_SOURCES := src/quicksand.c src/time.c $(TEST_SOURCES)
TIMESTAMP_CFLAGS := $(filter-out $(SANITIZER_CFLAGS),$(CFLAGS))

.PHONY: all test check check-slow check-runtime check-analysis clean docs install-lib install-docs install uninstall-lib uninstall-docs uninstall python tidy clang-analyze fanalyzer cppcheck sanitize-check analysis

all: $(LIB_SHARED) $(LIB_STATIC) $(TEST_BINS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test:
	mkdir -p $(BUILD_DIR)/test

$(DOC_BUILD_DIR):
	mkdir -p $(DOC_BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/timestamp.o: $(TIMESTAMP_SRC) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(TIMESTAMP_CFLAGS) -c $< -o $@

$(LIB_SHARED): $(LIB_OBJECTS)
	$(CC) -shared -o $@ $(LIB_OBJECTS) $(LDFLAGS) $(LDLIBS)

$(LIB_STATIC): $(LIB_OBJECTS)
	$(AR) rcs $@ $(LIB_OBJECTS)

$(BUILD_DIR)/test/%: test/test_%.c $(LIB_STATIC) | $(BUILD_DIR)/test
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_STATIC) $(LDFLAGS) $(LDLIBS) -o $@

test: $(TEST_BINS)

check-runtime: $(BUILD_DIR)/test/basic $(BUILD_DIR)/test/time
	./$(BUILD_DIR)/test/basic
	./$(BUILD_DIR)/test/time

check-analysis: compile_commands.json
	@set +e; \
	for target in tidy clang-analyze fanalyzer cppcheck; do \
		echo "== $$target (warnings only) =="; \
		$(MAKE) --no-print-directory $$target || \
			echo "$$target reported findings; continuing"; \
	done

# Keep runtime suites and benchmarks sequential, even with make -j.
check: check-runtime
	$(MAKE) sanitize-check
	$(MAKE) bench-transport

check-slow: check
	$(MAKE) check-analysis

compile_commands.json: build.ninja
	$(NINJA) -f $< -t compdb cc cc_timestamp link_exe > $@

tidy: compile_commands.json
	$(CLANG_TIDY) -quiet --extra-arg=-Wno-unused-command-line-argument -p . $(ANALYZE_SOURCES)

clang-analyze:
	set -e; \
	for src in $(ANALYZE_SOURCES); do \
		echo "CLANG-ANALYZE $$src"; \
		$(CLANG) $(CPPFLAGS) $(CFLAGS) --analyze -Xanalyzer -analyzer-output=text $$src; \
	done

fanalyzer:
	set -e; \
	for src in $(ANALYZE_SOURCES); do \
		echo "FANALYZER $$src"; \
		$(GCC_ANALYZER) $(CPPFLAGS) $(CFLAGS) -fanalyzer -c $$src -o /dev/null; \
	done

cppcheck: compile_commands.json
	$(CPPCHECK) --project=$< $(CPPCHECK_FLAGS) --error-exitcode=1

sanitize-check:
	rm -rf $(SANITIZE_BUILD_DIR)
	ASAN_OPTIONS=detect_leaks=1:strict_init_order=1:check_initialization_order=1 \
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	$(MAKE) BUILD_DIR=$(SANITIZE_BUILD_DIR) CC=$(CLANG) \
		CFLAGS='$(CFLAGS) $(SANITIZER_CFLAGS)' \
		LDFLAGS='$(LDFLAGS) $(SANITIZER_LDFLAGS)' \
		check-runtime

analysis: check-analysis

$(DOC_BUILD_DIR)/.stamp: $(DOC_FILES) | $(DOC_BUILD_DIR)
	cp $(DOC_FILES) $(DOC_BUILD_DIR)/
	touch $@

$(MAN_PAGE): $(MAN_SOURCE) | $(DOC_BUILD_DIR)
	$(SCDOC) < $< > $@

docs: $(DOC_BUILD_DIR)/.stamp $(MAN_PAGE)

$(PC_FILE): $(DOC_DIR)/quicksand.pc.in | $(BUILD_DIR)
	sed \
		-e 's|@PREFIX@|$(PREFIX)|g' \
		-e 's|@LIBDIR@|$(PREFIX)/lib|g' \
		-e 's|@INCLUDEDIR@|$(PREFIX)/include|g' \
		-e 's|@VERSION@|$(PACKAGE_VERSION)|g' \
		$< > $@

install-lib: $(LIB_SHARED) $(LIB_STATIC) $(PC_FILE)
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m 0644 $(LIB_SHARED) $(DESTDIR)$(PREFIX)/lib/
	install -m 0644 $(LIB_STATIC) $(DESTDIR)$(PREFIX)/lib/
	install -m 0644 include/quicksand.h $(DESTDIR)$(PREFIX)/include/
	install -m 0644 include/quicksand_style.h $(DESTDIR)$(PREFIX)/include/
	install -m 0644 $(PC_FILE) $(DESTDIR)$(PREFIX)/lib/pkgconfig/

install-docs: docs
	install -d $(DESTDIR)$(PREFIX)/share/doc/quicksand
	install -d $(DESTDIR)$(PREFIX)/share/man/man3
	install -m 0644 $(DOC_FILES) $(DESTDIR)$(PREFIX)/share/doc/quicksand/
	install -m 0644 $(MAN_PAGE) $(DESTDIR)$(PREFIX)/share/man/man3/

install: install-lib install-docs

uninstall-lib:
	rm -f $(DESTDIR)$(PREFIX)/lib/libquicksand.so
	rm -f $(DESTDIR)$(PREFIX)/lib/libquicksand.a
	rm -f $(DESTDIR)$(PREFIX)/include/quicksand.h
	rm -f $(DESTDIR)$(PREFIX)/include/quicksand_style.h
	rm -f $(DESTDIR)$(PREFIX)/lib/pkgconfig/quicksand.pc

uninstall-docs:
	rm -f $(addprefix $(DESTDIR)$(PREFIX)/share/doc/quicksand/,$(DOC_BASENAMES))
	rm -f $(DESTDIR)$(PREFIX)/share/man/man3/quicksand.3
	rmdir $(DESTDIR)$(PREFIX)/share/doc/quicksand 2>/dev/null || true

uninstall: uninstall-lib uninstall-docs

python: $(LIB_SHARED)
	cd lang/python && { pip install --break-system-packages -e .; } || { pip install -e .; }

clean:
	rm -rf $(BUILD_DIR) $(SANITIZE_BUILD_DIR)
	rm -f compile_commands.json

$(BUILD_DIR)/quicksand.o: src/quicksand_crc.h include/quicksand.h include/quicksand_style.h

.PHONY: bench-copy
bench-copy: $(BUILD_DIR)/test/bench_copy
	./$(BUILD_DIR)/test/bench_copy

$(BUILD_DIR)/test/bench_copy: test/bench_copy.c src/quicksand_crc.h $(LIB_STATIC) | $(BUILD_DIR)/test
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_STATIC) $(LDFLAGS) $(LDLIBS) -o $@

.PHONY: bench-transport
bench-transport: $(BUILD_DIR)/test/bench_transport
	./$(BUILD_DIR)/test/bench_transport

$(BUILD_DIR)/test/bench_transport: test/bench_transport.c include/quicksand.h $(LIB_STATIC) | $(BUILD_DIR)/test
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $< $(LIB_STATIC) $(LDFLAGS) $(LDLIBS) -o $@
