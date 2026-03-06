CC ?= cc
AR ?= ar
PREFIX ?= /usr/local
ARCH ?= $(shell uname -m)

BUILD_DIR := build
LIB_SHARED := $(BUILD_DIR)/libquicksand.so
LIB_STATIC := $(BUILD_DIR)/libquicksand.a

CPPFLAGS ?= -Iinclude
CFLAGS ?= -O2 -g -Wall -Wextra -std=c11 -fPIC
LDFLAGS ?=
LDLIBS ?= -lm

ifeq ($(ARCH),arm64)
ARCH := aarch64
endif

TIMESTAMP_SRC := src/timestamp+$(ARCH).s

ifeq ($(wildcard $(TIMESTAMP_SRC)),)
$(error Unsupported ARCH "$(ARCH)" (missing $(TIMESTAMP_SRC)))
endif

LIB_OBJECTS := $(BUILD_DIR)/quicksand.o $(BUILD_DIR)/time.o $(BUILD_DIR)/timestamp.o

TEST_SOURCES := test/test_basic.c test/test_time.c test/test_pub.c test/test_sub.c
TEST_BINS := $(patsubst test/test_%.c,$(BUILD_DIR)/test/%,$(TEST_SOURCES))

.PHONY: all test check clean install uninstall python

all: $(LIB_SHARED) $(LIB_STATIC)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/test:
	mkdir -p $(BUILD_DIR)/test

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/timestamp.o: $(TIMESTAMP_SRC) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(LIB_SHARED): $(LIB_OBJECTS)
	$(CC) -shared -o $@ $(LIB_OBJECTS) $(LDFLAGS) $(LDLIBS)

$(LIB_STATIC): $(LIB_OBJECTS)
	$(AR) rcs $@ $(LIB_OBJECTS)

test: $(TEST_BINS)

$(BUILD_DIR)/test/%: test/test_%.c $(LIB_STATIC) | $(BUILD_DIR)/test
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_STATIC) $(LDFLAGS) $(LDLIBS) -o $@

check: $(BUILD_DIR)/test/basic $(BUILD_DIR)/test/time
	./$(BUILD_DIR)/test/basic
	./$(BUILD_DIR)/test/time

install: all
	install -d $(PREFIX)/lib $(PREFIX)/include
	install -m 0644 $(LIB_SHARED) $(PREFIX)/lib/
	install -m 0644 $(LIB_STATIC) $(PREFIX)/lib/
	install -m 0644 include/quicksand.h $(PREFIX)/include/
	install -m 0644 include/quicksand_style.h $(PREFIX)/include/

uninstall:
	rm -f $(PREFIX)/lib/libquicksand.so
	rm -f $(PREFIX)/lib/libquicksand.a
	rm -f $(PREFIX)/include/quicksand.h
	rm -f $(PREFIX)/include/quicksand_style.h

python: $(LIB_SHARED)
	cd lang/python && pip install .

clean:
	rm -rf $(BUILD_DIR)
