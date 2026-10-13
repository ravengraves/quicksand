#include "quicksand.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

/* Substitute the clock to deterministically schedule recovery just before
 * publication, without adding hooks to the library or relying on sleeps. */
static uint64_t ticks = 1000000000;
static unsigned pause_at;
static unsigned replacements;
static quicksand_connection *connection;

static void recover_and_publish(void) {
	uint8_t message = 'B';
	int64_t rc = quicksand_write(connection, &message, 1);
	assert(rc == -ETIMEDOUT); // Waiting for the paused writer locks the ring.
	rc = quicksand_write(connection, &message, 1);
	assert(rc == -ETIMEDOUT); // Recover and reset reservations.
	assert(connection->buffer->locked == 0);
	for(unsigned i = 0; i < replacements; ++i) {
		message = (uint8_t) ('C' + i);
		rc = quicksand_write(connection, &message, 1);
		assert(rc == 0);
	}
	assert(connection->buffer->index == replacements);
}

uint64_t quicksand_now(void) {
	ticks += 100000000;
	uint64_t now = ticks;
	if(pause_at && --pause_at == 0) {
		recover_and_publish();
	}
	return now;
}

double quicksand_ns(uint64_t end, uint64_t start) {
	return end >= start ? (double) (end - start) : -(double) (start - end);
}

int main(void) {
	for(replacements = 1; replacements <= 2; ++replacements) {
		quicksand_delete("test_recovery", -1);
		int64_t rc = quicksand_connect(&connection, "test_recovery", -1, 1, 8, NULL);
		assert(rc == 0);
		/* Calls: entry timestamp, preamble timestamp, publication timestamp.
		 * At the third call, the old writer has passed its index wait. */
		pause_at = 3;
		uint8_t message = 'A';
		rc = quicksand_write(connection, &message, 1);
		assert(rc == -ETIMEDOUT);
		assert(connection->buffer->index == replacements);
		assert(connection->buffer->locked == 0);

		// A failed stale publication must leave the recovered ring usable.
		message = 'Z';
		rc = quicksand_write(connection, &message, 1);
		assert(rc == 0);
		assert(connection->buffer->index == replacements + 1);
		connection->read_stamp = connection->buffer->updatestamp;
		for(unsigned i = 0; i <= replacements; ++i) {
			int64_t length = 1;
			rc = quicksand_read(connection, &message, &length);
			assert(rc == (int64_t) (replacements - i));
			assert(length == 1 && message == (i == replacements ? 'Z' : 'C' + i));
		}
		quicksand_disconnect(&connection, NULL);
		quicksand_delete("test_recovery", -1);
	}
	puts("Timeout recovery publication tests passed");
}
