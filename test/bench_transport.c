#define _POSIX_C_SOURCE 200809L
#include "quicksand.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct test {
	quicksand_connection *writer, *reader;
	size_t size;
	uint64_t start, end;
	_Alignas(64) uint64_t sent;
	_Alignas(64) uint64_t received;
	uint64_t rejected;
	double latency;
	_Alignas(64) atomic_int ready, go, done;
};

static void check(int ok, const char *operation) {
	if(!ok) {
		fprintf(stderr, "%s failed\n", operation);
		exit(EXIT_FAILURE);
	}
}

static uint8_t *prepare(struct test *t) {
	uint8_t *message = malloc(t->size);
	check(message != NULL, "malloc");
	memset(message, 0, t->size);
	atomic_fetch_add(&t->ready, 1);
	while(!atomic_load(&t->go)) {}
	return message;
}

static void *write_messages(void *arg) {
	struct test *t = arg;
	uint8_t *message = prepare(t);
	do {
		uint64_t timestamp = quicksand_now();
		memcpy(message, &timestamp, sizeof(timestamp));
		check(quicksand_write(t->writer, message, (int64_t) t->size) == 0, "write");
		++t->sent;
	} while((t->sent & 255) || quicksand_elapsed(t->start) < 1e9);
	atomic_store(&t->done, 1);
	free(message);
	return NULL;
}

static void *read_messages(void *arg) {
	struct test *t = arg;
	uint8_t *message = prepare(t);
	int finished = 0;
	for(;;) {
		int64_t size = (int64_t) t->size;
		int64_t rc = quicksand_read(t->reader, message, &size);
		if(rc >= 0) {
			uint64_t received = quicksand_now(), sent;
			memcpy(&sent, message, sizeof(sent));
			check(size == (int64_t) t->size, "read size");
			t->latency += quicksand_ns(received, sent);
			++t->received;
		} else if(rc == -EBADMSG) {
			++t->rejected;
		} else {
			check(rc == -1, "read");
			if(finished) {
				break;
			}
			/* Retry after observing completion so the final publication is drained. */
			finished = atomic_load(&t->done);
		}
	}
	t->end = quicksand_now();
	free(message);
	return NULL;
}

int main(void) {
	const size_t sizes[] = {8, 512, 1024, 16384, 1048576};
	quicksand_ns_calibrate(10000000);
	for(size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
		struct test t = {.size = sizes[i]};
		atomic_init(&t.ready, 0);
		atomic_init(&t.go, 0);
		atomic_init(&t.done, 0);
		char topic[64];
		snprintf(topic, sizeof(topic), "qs_bench_%ld_%zu", (long) getpid(), t.size);
		/* Largest power-of-two capacity fitting within 32 MiB, including metadata. */
		size_t header = (sizeof(quicksand_ringbuffer) + 63) & ~(size_t) 63;
		size_t stride = (t.size + 20 + 63) & ~(size_t) 63;
		int64_t slots = 2;
		while(header + (size_t) slots * 2 * stride <= 32 * 1024 * 1024) {
			slots *= 2;
		}
		check(quicksand_connect(&t.writer, topic, -1, (int64_t) t.size, slots, NULL) == 0, "writer connect");
		int64_t rc = quicksand_connect(&t.reader, topic, -1, -1, -1, NULL);
		/* POSIX mappings remain valid after unlink; no named ring is left on failure. */
		quicksand_delete(topic, -1);
		check(rc == 0, "reader connect");
		memset((uint8_t *) t.writer->buffer + header, 0, (size_t) slots * stride);
		pthread_t writer, reader;
		check(pthread_create(&writer, NULL, write_messages, &t) == 0, "writer thread");
		check(pthread_create(&reader, NULL, read_messages, &t) == 0, "reader thread");
		while(atomic_load(&t.ready) != 2) {}
		t.start = quicksand_now();
		atomic_store(&t.go, 1);
		check(pthread_join(writer, NULL) == 0, "writer join");
		check(pthread_join(reader, NULL) == 0, "reader join");
		check(t.received != 0, "no messages received");
		double elapsed = quicksand_ns(t.end, t.start);
		int64_t rate = (int64_t) (t.received * 1e9 / elapsed);
		printf("%zuB: %" PRId64 " msgs/s, %.2f ns/msg, %.2f ns latency, %.2f%% skip, %" PRIu64 " CRC rejects\n",
		       t.size, rate, elapsed / t.received, t.latency / t.received,
		       100.0 * (t.sent - t.received) / t.sent, t.rejected);
		quicksand_disconnect(&t.reader, NULL);
		quicksand_disconnect(&t.writer, NULL);
	}
	return 0;
}
