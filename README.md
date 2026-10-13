# Quicksand 🏜️

A multiple-reader multiple-writer communication library enabling robots to think at the speed of silicon!

We have benchmarked sending over 15 million messages per second per process on a on a 2020 laptop processor (i7-1165G7 @ 2.80GHz). This library uses a lock-free shared-memory ring buffer in order to perform inter-process message passing significantly faster than other libraries.

Quicksand is a low-latency message passing transport, but the Quicksand protocol does not guarantee that messages were received. Old messages are automatically dropped, and timing delays from an overloaded CPU can also cause messages to be dropped. Users must implement higher-level protocols if they need to ensure any single message was received.

## Usage - Python 🐍

Publisher - Python
```Python
import quicksand as qs

qs.delete("hi_topic")
c = qs.connection("hi_topic", 8, 1000000)

while True:
    c.write(b"hello")
```

Subscriber - Python:
```Python
import quicksand as qs

c = qs.connection("hi_topic", -1, -1)
count = 0
start = qs.now()

c.read_latest()
while True:
    for msg in c:
        if msg ==  b"hello":
            count += 1
    dt = qs.ns_elapsed(start)
    if dt > 1e9:
        print(count / (dt * 1e-9), "msgs/s")
        count = 0
        start = qs.now()
```

Output (Python 3.10, Clang 14.0.0, i7-1165G7 @ 2.80GHz):
```
1136568.6862551533 msgs/s
1524979.1982293406 msgs/s
1536257.701024759 msgs/s
1516702.770531038 msgs/s
1517196.7936193675 msgs/s
1500912.66844739 msgs/s
```

## Usage - C 🚀

Publisher example - C
```C
#include <assert.h>
#include <signal.h>
#include <stdio.h>

#include "quicksand.h"


static volatile int ok = 1;

void interrupt()
{
	ok = 0;
}

int main()
{
	signal(SIGINT, interrupt);
	quicksand_connection *writer = NULL;
	int64_t rate = 1000000; // 1 million msgs/s
	int64_t success = quicksand_connect(&writer, "test_pubsub", -1, 8, rate, NULL);
	assert(success == 0 && writer);

	int32_t data = 0;

	double delay = 1.0 / (f64) rate * 1e9;
	while(ok) {
		uint64_t start = quicksand_now();
		int64_t ret = quicksand_write(writer, (uint8_t *) &data, sizeof(data));
		if(ret != 0) {
			continue;
		}
		data = (data + 1) & (32768 - 1); // modulo
		quicksand_sleep(delay - quicksand_ns(quicksand_now(), start));
	}

	quicksand_disconnect(&writer, NULL);
	quicksand_delete("test_pubsub", -1);
	return 0;
}
```


Subscriber example - C
```C
#include <assert.h>
#include <signal.h>
#include <stdio.h>

#include "quicksand.h"

static volatile int ok = 1;

void interrupt()
{
	ok = 0;
}

int main()
{
	signal(SIGINT, interrupt);
	quicksand_connection *reader = NULL;
	int64_t disconnected = 1;
	while(ok && disconnected) {
		disconnected = quicksand_connect(&reader, "test_pubsub", -1,
				-1, -1, NULL);
	}
	assert(disconnected == 0 && reader);

	uint64_t start = quicksand_now();
	uint64_t count = 0;
	uint64_t skipcount = 0;
	int32_t data = 0;
	int64_t size = sizeof(data);
	int32_t last = 0;
	while(ok) {
		int64_t ret = quicksand_read(reader, (uint8_t *) &data, &size);
		if(ret > -1) {
			count += 1;
			skipcount += (uint64_t) (data != ((last + 1) & (32768 - 1)));
			last = data;
		}

		double ns = quicksand_ns(quicksand_now(), start);
		if(ns > 1e9) {
			printf("%f msgs/s (drop: %f %%)\n",
			       (double) count / (ns * 1e-9),
			       (double) skipcount / (double) count * 100.0);
			count = 0;
			skipcount = 0;
			start = quicksand_now();
		}
	}

	quicksand_disconnect(&reader, NULL);
	return 0;
}
```


Running:

* Terminal 1:
```
make build/test/pub
make build/test/sub
./build/test/sub
```
* Terminal 2:
```
./build/test/pub
```

* Terminal 3:
```
./build/test/pub
```

Output:
```
$ ./build/test/sub
999397.986483 msgs/s (drop: 0.000100 %)
1003744.982127 msgs/s (drop: 0.000000 %)
1005704.982092 msgs/s (drop: 0.000000 %)
1749063.970103 msgs/s (drop: 83.113197 %)
1998851.997210 msgs/s (drop: 97.216402 %)
1998569.991506 msgs/s (drop: 97.370070 %)
```
*(Note: the Drop % calculation assumes one publisher.)*

Removing the sleep from the test_pub.c and running with only one publisher can result in the following messaging rate using Clang 14.0.0 on a 2020 laptop processor (i7-1165G7 @ 2.80GHz):

```
$ ./build/test/sub
18603454.891196 msgs/s (drop: 0.000005 %)
20161391.342656 msgs/s (drop: 0.000000 %)
20459358.354837 msgs/s (drop: 0.000005 %)
19552304.997248 msgs/s (drop: 0.000000 %)
20273548.563203 msgs/s (drop: 0.000000 %)
20429499.443235 msgs/s (drop: 0.000000 %)
```

## Message integrity

Ring protocol version 1 keeps the 192-byte ring header and all existing field
offsets, placing a `uint32_t version` in its padding. Each slot contains
`timestamp:8 | payload_length:8 | payload:N | CRC32C:4 | padding` in native byte
order. Strides reserve 20 bytes of overhead and round up to 64 bytes, with a
minimum of 64 bytes. CRC-32C covers the entire 16-byte preamble and every payload
byte, excluding the trailer and padding.

A version mismatch returns `-EPROTONOSUPPORT` on connection. Upgrade all processes
together: stop old processes and recreate topics. Old binaries before version 1 
do not reliably reject the new format. CRC detects errors; it adds no
synchronization and does not guarantee detection of concurrent overwrites. 

A corrupt length or checksum returns `-EBADMSG` and consumes the selected message.
The caller's length stays unchanged on failure; discard any copied bytes.
An undersized output buffer still returns `-EINVAL` without consuming the selected
message, allowing retry subject to the usual overwrite/lag policy.

The private copy helper uses four 128-bit streams when the compile target enables
x86-64 SSE4.2/PCLMUL or little-endian AArch64 NEON/PMULL/CRC. Other targets use a
static slicing-by-4 tables (4 KiB total) with fused four-byte copying and a
bytewise remainder. This exposes independent table lookups without requiring
SIMD instructions or runtime initialization.
Make selects the native CPU by default; Ninja's default flags select the portable fallback. 
Set target flags explicitly for deployment on other CPUs.
Define `QUICKSAND_CRC_SOFTWARE` to force the fallback, for example:

```sh
make check-runtime
make BUILD_DIR=build-software CPPFLAGS='-Iinclude -DQUICKSAND_CRC_SOFTWARE' check-runtime
make BUILD_DIR=build-generic CFLAGS='-O3 -g -Wall -Wextra -std=c11 -fPIC' check-runtime
```

## Copy benchmark

Run `make bench-copy`, `make -f Makefile.darwin bench-copy`, or `ninja bench-copy`.
It compares the production copy plus CRC against libc `memcpy` for 8 B, 512 B,
1 KiB, 16 KiB, and 1 MiB. Alignments cover aligned buffers, ring writes/reads with
a +16 offset, and unaligned buffers.

## Transport benchmark

Run `make bench-transport` (macOS: `make -f Makefile.darwin bench-transport`)
or `ninja bench-transport`.

A writer thread and a busy-polling reader thread start on a shared signal. Each test writes unthrottled for about one second. Allocation and ring prefaulting happen before timing. Each output line reports payload size, successful reads per second, nanoseconds per successful read, average delivery latency, skip percentage, and CRC rejection count (included in skips).

Latency runs from the timestamp placed in the first eight payload bytes just
before `quicksand_write` to completion of `quicksand_read`, including both copies
and queueing. Timestamping overhead is included; the timestamp is part of the
stated payload size. Latency covers only received messages. Thread placement is left to the OS.

## Installation 📖

Install library:
```
make
make check
sudo make install
```

Python install (after library install):
```
cd lang/python && pip install .
```

## Analysis

Static analysis and sanitizer targets are available from the repo root:
```
make compile_commands.json
make tidy
make clang-analyze
make fanalyzer
make sanitize-check
make analysis
```

`make sanitize-check` rebuilds into `build-sanitize/` with Clang ASan+UBSan and reruns `make check-runtime`, so the normal `build/` artifacts remain untouched.


## Changes
---
**2026-10-13:** added CRC32 to reject overlapping reads from delayed writer.

Measured on 2026-10-13, Intel Core i7-1165G7, GCC 14.2.0,
`-O3 -march=native -mtune=native`, x86 PCLMUL/CRC backend.

Comparing the non-crc `3a569e3` baseline with protocol version 1 (CRC):

| Payload | Reads/s before → CRC | Change | Delivery latency µs before → CRC | Skip % before → CRC |
| --- | ---: | ---: | ---: | ---: |
| 8 B | 13,347,620 → 11,692,027 | -12.4% | 1.91 → 0.25 | 0.00 → 0.00 |
| 512 B | 8,325,154 → 7,665,717 | -7.9% | 16.46 → 5.18 | 0.00 → 0.00 |
| 1 KiB | 8,007,201 → 7,406,522 | -7.5% | 51.15 → 44.92 | 0.53 → 0.37 |
| 16 KiB | 840,845 → 832,903 | -0.9% | 5.62 → 7.17 | 0.29 → 0.57 |
| 1 MiB | 13,471 → 10,672 | -20.8% | 160.71 → 375.34 | 2.42 → 39.24 |

Latency can double for large payloads but is unchanged (or improved, perhaps due to cache) for small payloads. Max throughput is lowered by 10-20% depending on payload size. Invalid data from conflicting writers are rejected.

---

## License ⚖

MIT License


