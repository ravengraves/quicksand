QUICKSAND(3) "Quicksand" "Quicksand Manual"

# NAME

quicksand - lock-free shared-memory ringbuffer messaging library

# SYNOPSIS

*#include <quicksand.h>*

```
int64_t quicksand_connect(quicksand_connection **connection, char *topic,
	int64_t topic_length, int64_t message_size, int64_t message_rate,
	void *alloc);
void quicksand_disconnect(quicksand_connection **connection, void *dealloc);
void quicksand_delete(char *topic, int64_t topic_length);
int64_t quicksand_write(quicksand_connection *connection, uint8_t *message,
	int64_t message_size);
int64_t quicksand_read(quicksand_connection *connection, uint8_t *message,
	int64_t *message_size);
uint64_t quicksand_now(void);
double quicksand_ns(uint64_t final_timestamp, uint64_t initial_timestamp);
double quicksand_ns_calibrate(double nanoseconds);
void quicksand_sleep(double nanoseconds);
```

# DESCRIPTION

Quicksand provides high-throughput inter-process message passing through a
shared-memory ring buffer. It is designed for multiple readers and writers and
exposes a small C API for connecting to a named topic, writing fixed-size
messages, and reading the newest available data.

Each topic is backed by shared memory. Writers create or attach to the topic
with an explicit slot size and rate. Readers attach to an existing topic and
consume messages in FIFO order, or can jump to the latest message with the
inline helper in _quicksand.h_.

# FUNCTIONS

*quicksand_connect*()
	Create or attach to a topic. Pass _topic_length_ as -1 to use
	*strlen*(3). Readers typically pass -1 for both _message_size_ and
	_message_rate_ to attach to an existing topic. Creation rounds capacity
	up to a power of two with at least two slots. Unrepresentable allocation
	sizes return -EOVERFLOW. Slots reserve a 16-byte preamble and a 4-byte
	CRC-32C trailer, rounded up to at least 64 bytes. Incompatible ring
	versions return -EPROTONOSUPPORT.

*quicksand_disconnect*()
	Unmap shared memory and release the connection object.

*quicksand_delete*()
	Remove the named topic so it can be recreated cleanly.

*quicksand_write*()
	Write one message to the ring buffer. Returns 0 on success or a negative
	error code. Returns -ETIMEDOUT if timeout recovery overtakes the writer's
	reservation before publication.

*quicksand_read*()
	Read the next available message into the caller buffer. Returns the number
	of remaining unread messages, -1 when no message is available, or a
	negative error code. If the output buffer is too small, returns -EINVAL
	without consuming the selected message. Retry with a larger buffer;
	the usual overwrite and reader-lag policy still applies. Corrupt lengths
	or CRC-32C mismatches return -EBADMSG and consume the selected message.
	On failure the caller's length is unchanged; discard any output bytes.

*quicksand_now*(), *quicksand_ns*(), *quicksand_ns_calibrate*(),
*quicksand_sleep*()
	Low-level timing helpers used by Quicksand and by latency-sensitive
	callers.

# PROTOCOL UPGRADES

Ring version 1 stores a version marker in existing ring-header padding. Message
slots retain the 16-byte timestamp/length preamble followed by the payload and a
4-byte CRC-32C trailer. The checksum covers the preamble and every payload byte.

Stop all old processes and recreate topics during an upgrade. Old binaries do
not reliably reject this format. CRC provides error detection, not synchronization
or guaranteed overwrite detection. Readers poll with relaxed ordering, then
acquire the published index before selecting and copying a slot.

# EXAMPLE

```
#include <stdint.h>
#include <stdio.h>
#include "quicksand.h"

int main(void) {
	quicksand_connection *writer = NULL;
	quicksand_connection *reader = NULL;
	uint8_t payload[4] = {1, 2, 3, 4};
	uint8_t recv[4] = {0};
	int64_t recv_size = sizeof(recv);

	quicksand_delete("example", -1);
	quicksand_connect(&writer, "example", -1, 32, 1000, NULL);
	quicksand_connect(&reader, "example", -1, -1, -1, NULL);

	quicksand_write(writer, payload, sizeof(payload));
	if(quicksand_read(reader, recv, &recv_size) >= 0) {
		printf("read %lld bytes\n", (long long) recv_size);
	}

	quicksand_disconnect(&reader, NULL);
	quicksand_disconnect(&writer, NULL);
	quicksand_delete("example", -1);
	return 0;
}
```

Compile against the installed library with pkg-config:

```
cc -O2 example.c $(pkg-config --cflags --libs quicksand)
```

For a publisher/subscriber smoke test using the repository test programs:

```
make all
./build/test/sub
./build/test/pub
```

# BUILD

The library build is intentionally simple:

- *make all* builds the shared library, static library, and test executables.
- *make check* runs the runtime tests, ASan/UBSan tests, and transport benchmark.
- *make check-slow* also runs static analyzers, including cppcheck.
- *make docs* generates documentation, including this man page via *scdoc*(1).

For pkg-config consumers, *make install-lib* installs _quicksand.pc_.

# FILES

_quicksand.h_
	Public API.

_quicksand_style.h_
	Shared integer and floating-point type aliases.

_quicksand.pc_
	pkg-config metadata installed by *make install-lib*.

# SEE ALSO

*scdoc*(1), *pkg-config*(1)
