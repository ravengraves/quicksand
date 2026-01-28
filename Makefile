CC := clang
AR := llvm-ar

# x86_64,aarch64
ARCH?=$(shell uname -m)
PREFIX?=/usr/local

# Debug flags
# CFLAGS := -Wall -Wextra -Wpedantic -Werror -std=c11 -fPIC -flto \
# 	-fstack-protector-all -D_FORTIFY_SOURCE=3 \
# 	-ffunction-sections -fdata-sections -march=native -mtune=native \
# 	-I quicksand/include -O3 -g -ffast-math -fsanitize=address,undefined
# LDFLAGS := -flto -fsanitize=address,undefined
# CEXE := -fuse-ld=lld

# Base flags without LTO
CFLAGS_BASE := -Wall -Wextra -Wpedantic -Werror -std=c11 -fPIC \
	-fno-stack-protector -D_FORTIFY_SOURCE=0 \
	-ffunction-sections -fdata-sections \
	-I quicksand/include -O3 -ffast-math -mtune=native

# LTO flags for static library only (executable linking)
CFLAGS_LTO := $(CFLAGS_BASE) -flto

# Non-LTO for shared library (Python/dlopen compatibility)
CFLAGS_SO := $(CFLAGS_BASE)

# Legacy CFLAGS for tests (with LTO)
CFLAGS := $(CFLAGS_LTO)

CEXE := -fuse-ld=lld -Wl,-gc-sections,-icf=safe,-s
LDFLAGS := -fuse-ld=lld

ifeq ($(ARCH),x86_64)
	CFLAGS_BASE += -march=native
	CFLAGS_LTO += -march=native
	CFLAGS_SO += -march=native
	CFLAGS += -march=native
endif

ifeq ($(ARCH),aarch64)
	CFLAGS_BASE += -march=armv8-a
	CFLAGS_LTO += -march=armv8-a
	CFLAGS_SO += -march=armv8-a
	CFLAGS += -march=armv8-a
endif

all: build/libquicksand.so build/libquicksand.a

# Shared library - no LTO for Python/dlopen compatibility
build/libquicksand.so: build/quicksand_now.o build/quicksand_time_so.o build/quicksand_so.o
	$(CC) -shared -o build/libquicksand.so \
		build/quicksand_now.o \
		build/quicksand_time_so.o \
		build/quicksand_so.o \
		$(LDFLAGS)

# Static library - with LTO for best performance in C programs
build/libquicksand.a: build/quicksand_now.o build/quicksand_time_lto.o build/quicksand_lto.o
	$(AR) rcs build/libquicksand.a \
		build/quicksand_now.o \
		build/quicksand_time_lto.o \
		build/quicksand_lto.o

build/quicksand_now.o: quicksand/src/timestamp+$(ARCH).s
	mkdir -p build
	$(CC) -c -o build/quicksand_now.o -fPIC quicksand/src/timestamp+$(ARCH).s

# Non-LTO objects for shared library
build/quicksand_time_so.o: quicksand/src/time.c
	mkdir -p build
	$(CC) -c -o build/quicksand_time_so.o $(CFLAGS_SO) quicksand/src/time.c

build/quicksand_so.o: quicksand/src/quicksand.c
	mkdir -p build
	$(CC) -c -o build/quicksand_so.o $(CFLAGS_SO) quicksand/src/quicksand.c

# LTO objects for static library
build/quicksand_time_lto.o: quicksand/src/time.c
	mkdir -p build
	$(CC) -c -o build/quicksand_time_lto.o $(CFLAGS_LTO) quicksand/src/time.c

build/quicksand_lto.o: quicksand/src/quicksand.c
	mkdir -p build
	$(CC) -c -o build/quicksand_lto.o $(CFLAGS_LTO) quicksand/src/quicksand.c


### TESTS ###

build/test/basic: build/libquicksand.a test/test_basic.c
	mkdir -p build/test
	$(CC) -o build/test/basic test/test_basic.c $(CFLAGS) $(CEXE) \
		build/libquicksand.a
		# (shared:) -L build -lquicksand

build/test/time: build/libquicksand.a test/test_time.c
	mkdir -p build/test
	$(CC) -o build/test/time test/test_time.c $(CFLAGS) $(CEXE) \
		build/libquicksand.a

build/test/pub: build/libquicksand.a test/test_pub.c
	mkdir -p build/test
	$(CC) -o build/test/pub test/test_pub.c $(CFLAGS) $(CEXE) \
		build/libquicksand.a

build/test/sub: build/libquicksand.a test/test_sub.c
	mkdir -p build/test
	$(CC) -o build/test/sub test/test_sub.c $(CFLAGS) $(CEXE) \
		build/libquicksand.a

check: build/test/basic build/test/time
	./build/test/time
	./build/test/basic

compile_commands.json: Makefile
	@echo '[\n' \
	'{"directory":"$(PWD)","command":"$(CC) -c -fPIC quicksand/src/timestamp+$(ARCH).s -o build/quicksand_now.o","file":"quicksand/src/timestamp+$(ARCH).s"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -c $(CFLAGS) quicksand/src/time.c -o build/quicksand_time.o","file":"quicksand/src/time.c"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -c $(CFLAGS) quicksand/src/quicksand.c -o build/quicksand.o","file":"quicksand/src/quicksand.c"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -shared -o build/libquicksand.so build/quicksand_now.o build/quicksand_time.o build/quicksand.o $(LDFLAGS)","file":"build/libquicksand.so"},\n' \
	'{"directory":"$(PWD)","command":"$(AR) rcs build/libquicksand.a build/quicksand_now.o build/quicksand_time.o build/quicksand.o","file":"build/libquicksand.a"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -o build/test/basic test/test_basic.c $(CFLAGS) $(CEXE) build/libquicksand.a","file":"test/test_basic.c"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -o build/test/time test/test_time.c $(CFLAGS) $(CEXE) build/libquicksand.a","file":"test/test_time.c"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -o build/test/pub test/test_pub.c $(CFLAGS) $(CEXE) build/libquicksand.a","file":"test/test_pub.c"},\n' \
	'{"directory":"$(PWD)","command":"$(CC) -o build/test/sub test/test_sub.c $(CFLAGS) $(CEXE) build/libquicksand.a","file":"test/test_sub.c"}\n]' \
	> $@

format:
	clang-format -style=file -i $$(find . | grep -e '\.c$$' -e '\.h$$')

install:
	mkdir -p $(PREFIX)/lib
	mkdir -p $(PREFIX)/include
	install -m 0644 build/libquicksand.so $(PREFIX)/lib/
	install -m 0644 build/libquicksand.a $(PREFIX)/lib/
	install -m 0644 quicksand/include/quicksand.h $(PREFIX)/include/
	sudo ldconfig

uninstall:
	rm $(PREFIX)/lib/libquicksand.so
	rm $(PREFIX)/lib/libquicksand.a
	rm $(PREFIX)/include/quicksand.h
	sudo ldconfig

python:
	cd lang/python && pip install .

clean:
	rm -rf build \
		lang/python/build \
		lang/python/*.whl \
		lang/python/*.egg-info \
		lang/python/quicksand/*.so

.PHONY: format all check clean python
