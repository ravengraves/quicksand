#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "quicksand.h"

/* Adding the version must preserve the existing shared-memory header ABI. */
_Static_assert(sizeof(quicksand_ringbuffer) == 192, "ring header size");
_Static_assert(offsetof(quicksand_ringbuffer, length) == 0, "length offset");
_Static_assert(offsetof(quicksand_ringbuffer, message_size) == 8, "slot size offset");
_Static_assert(offsetof(quicksand_ringbuffer, version) == 16, "version offset");
_Static_assert(offsetof(quicksand_ringbuffer, reserve) == 64, "reserve offset");
_Static_assert(offsetof(quicksand_ringbuffer, index) == 128, "index offset");
_Static_assert(offsetof(quicksand_ringbuffer, updatestamp) == 136, "stamp offset");
_Static_assert(offsetof(quicksand_ringbuffer, locked) == 144, "lock offset");

static void test_version(void) {
	quicksand_connection *c = NULL, *other = NULL;
	quicksand_delete("version", -1);
	assert(quicksand_connect(&c, "version", -1, 44, 8, NULL) == 0);
	assert(c->buffer->version == QUICKSAND_RING_VERSION);
	const uint32_t versions[] = {0, 2, UINT32_MAX};
	for(size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i) {
		c->buffer->version = versions[i];
		assert(quicksand_connect(&other, "version", -1, -1, -1, NULL) == -EPROTONOSUPPORT);
		assert(quicksand_connect(&other, "version", -1, 44, 8, NULL) == -EPROTONOSUPPORT);
		assert(quicksand_connect(&other, "version", -1, 45, 8, NULL) == -EPROTONOSUPPORT);
		assert(other == NULL);
	}
	c->buffer->version = QUICKSAND_RING_VERSION;
	assert(quicksand_connect(&other, "version", -1, -1, -1, NULL) == 0);
	quicksand_disconnect(&other, NULL);
	quicksand_disconnect(&c, NULL);
	quicksand_delete("version", -1);
}

static void test_slot_sizes(void) {
	const int64_t sizes[] = {1, 43, 44, 45, 48, 64, 108, 109};
	uint8_t input[192] = {0}, output[192];
	for(size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
		quicksand_connection *c = NULL;
		quicksand_delete("slot_sizes", -1);
		assert(quicksand_connect(&c, "slot_sizes", -1, sizes[i], 8, NULL) == 0);
		uint64_t stride = ((uint64_t) sizes[i] + 20 + 63) & ~UINT64_C(63);
		assert(c->buffer->message_size == stride && stride >= 64);
		int64_t capacity = (int64_t) stride - 20;
		assert(quicksand_write(c, input, capacity + 1) == -EMSGSIZE);
		assert(quicksand_write(c, input, -1) == -EMSGSIZE);
		assert(c->buffer->reserve == 0);
		assert(quicksand_write(c, input, capacity) == 0);
		assert(quicksand_write(c, input, 0) == 0);
		int64_t length = sizeof(output);
		assert(quicksand_read(c, output, &length) == 1);
		assert(length == capacity && memcmp(input, output, (size_t) length) == 0);
		length = sizeof(output);
		assert(quicksand_read(c, output, &length) == 0 && length == 0);
		quicksand_disconnect(&c, NULL);
		quicksand_delete("slot_sizes", -1);
	}
}

static void test_corruption(void) {
	quicksand_connection *c = NULL;
	quicksand_delete("corruption", -1);
	assert(quicksand_connect(&c, "corruption", -1, 5, 8, NULL) == 0);
	uint8_t input[5] = {1, 2, 3, 4, 5}, output[64];
	/* Flip every preamble, payload and (unaligned) trailer byte in turn.
	 * Length-byte zero changes 5 to 4, exercising a valid but wrong length. */
	for(size_t byte = 0; byte < 16 + sizeof(input) + 4; ++byte) {
		uint64_t selected = c->read_index;
		assert(quicksand_write(c, input, sizeof(input)) == 0);
		assert(quicksand_write(c, input, sizeof(input)) == 0);
		uint8_t *slot = (uint8_t *) c->buffer + sizeof(quicksand_ringbuffer)
				+ (selected & (c->buffer->length - 1)) * c->buffer->message_size;
		slot[byte] ^= 1;
		int64_t length = sizeof(output);
		assert(quicksand_read(c, output, &length) == -EBADMSG);
		assert(length == sizeof(output) && c->read_index == selected + 1);
		assert(quicksand_read(c, output, &length) == 0);
		assert(length == sizeof(input) && memcmp(input, output, sizeof(input)) == 0);
	}
	quicksand_disconnect(&c, NULL);
	quicksand_delete("corruption", -1);
}

static void *fail_alloc(size_t size) {
	(void) size;
	return NULL;
}

static void test_sizes(void) {
	quicksand_connection *c = NULL;
	// Header addition, cache-line rounding, power-of-two rounding, total size.
	assert(quicksand_connect(&c, "size_overflow", -1, INT64_MAX, 2, NULL) == -EOVERFLOW);
	assert(quicksand_connect(&c, "size_overflow", -1, INT64_MAX - 20, 2, NULL) == -EOVERFLOW);
	assert(quicksand_connect(&c, "size_overflow", -1, INT64_MAX - 83, 2, NULL) == -EOVERFLOW);
	assert(quicksand_connect(&c, "size_overflow", -1, 1, INT64_MAX, NULL) == -EOVERFLOW);
	assert(quicksand_connect(&c, "size_overflow", -1, 1, INT64_C(1) << 62, NULL) == -EOVERFLOW);
	assert(c == NULL);
}

static void test_half_ring(int64_t rate) {
	quicksand_connection *c = NULL;
	quicksand_delete("half_ring", -1);
	assert(quicksand_connect(&c, "half_ring", -1, 1, rate, NULL) == 0);
	assert(c->buffer->length == (uint64_t) (rate < 2 ? 2 : rate));
	uint64_t half = c->buffer->length / 2;
	uint8_t original = 0x11, replacement = 0x22, output;
	for(uint64_t i = 0; i < half; ++i) {
		assert(quicksand_write(c, &original, 1) == 0);
	}
	// Outstanding writers own half the ring. The next ticket must not wrap
	// onto the oldest readable slot when distance is exactly half a ring.
	c->buffer->reserve = c->buffer->index + half;
	assert(quicksand_write(c, &replacement, 1) == -ETIMEDOUT);
	for(uint64_t i = 0; i < half; ++i) {
		int64_t length = 1;
		assert(quicksand_read(c, &output, &length) >= 0);
		assert(length == 1 && output == original);
	}
	quicksand_disconnect(&c, NULL);
	quicksand_delete("half_ring", -1);
}

#ifndef _WIN32
static void test_posix_cleanup(void) {
	quicksand_connection *c = NULL;
	quicksand_delete("fd_zero", -1);
	assert(quicksand_connect(&c, "fd_zero", -1, 16, 8, NULL) == 0);
	pid_t child = fork();
	assert(child >= 0);
	if(child == 0) {
		// Force attachment to obtain descriptor zero without affecting parent.
		quicksand_connection *attached = NULL;
		int close_rc = close(STDIN_FILENO);
		assert(close_rc == 0 || errno == EBADF);
		assert(quicksand_connect(&attached, "fd_zero", -1, -1, -1, NULL) == 0);
		assert(attached->shared_memory_handle == 0);
		void *mapping = attached->buffer;
		size_t size = attached->shared_memory_size;
		quicksand_disconnect(&attached, NULL);
		assert(attached == NULL);
		errno = 0;
		assert(fcntl(STDIN_FILENO, F_GETFD) == -1 && errno == EBADF);
		errno = 0;
		assert(msync(mapping, size, MS_ASYNC) == -1 && errno == ENOMEM);
		// Preserve the actual shm_open error, rather than claiming ENOENT.
		struct rlimit limit;
		assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);
		limit.rlim_cur = 0;
		assert(setrlimit(RLIMIT_NOFILE, &limit) == 0);
		assert(quicksand_connect(&attached, "fd_zero", -1, -1, -1, NULL) == -EMFILE);
		_exit(0);
	}
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	quicksand_disconnect(&c, NULL);
	quicksand_delete("fd_zero", -1);
}
#endif

int main(void) {
	test_sizes();
	test_version();
	test_slot_sizes();
	test_corruption();
	test_half_ring(1);
	test_half_ring(2);
	test_half_ring(8);
#ifndef _WIN32
	test_posix_cleanup();
#endif
	quicksand_connection *writer = NULL;
	quicksand_connection *reader = NULL;
	quicksand_delete("test", -1);
	int64_t success1 = quicksand_connect(&writer, "test", -1, 32, 100, NULL);
	int64_t success2 = quicksand_connect(&reader, "test", -1, -1, -1, NULL);

	assert(success1 == 0 && writer);
	assert(success2 == 0 && reader);

	uint8_t data_write1[5] = {1, 2, 3, 4, 5};
	assert(quicksand_write(writer, data_write1, sizeof(data_write1)) == 0);
	uint8_t data_write2[5] = {6, 7, 8, 9, 10};
	assert(quicksand_write(writer, data_write2, sizeof(data_write2)) == 0);
	int64_t size = 5;
	uint8_t data_read1[size], data_read2[size];
	assert(quicksand_read_remaining(reader) == 2);
	uint64_t selected = reader->read_index;
	int64_t short_size = 1;
	assert(quicksand_read(reader, data_read1, &short_size) == -EINVAL);
	assert(short_size == 1 && reader->read_index == selected);
	int64_t read_ret1 = quicksand_read(reader, data_read1, &size);
	assert(read_ret1 == 1);
	assert(quicksand_read_remaining(reader) == 1);
	int64_t read_ret2 = quicksand_read(reader, data_read2, &size);
	assert(read_ret2 == 0);
	assert(quicksand_read_remaining(reader) == 0);

	for(int i = 0; i < size; i += 1) {
		assert(data_read1[i] == data_write1[i]);
		assert(data_read2[i] == data_write2[i]);
	}
	assert(quicksand_read(reader, data_read1, &size) == -1);

	// try to connect with wrong size and make sure it fails.
	quicksand_connection *writer_big = NULL;
	int64_t success3 = quicksand_connect(&writer_big, "test", -1, 32, 257, NULL);
	assert(success3 < 0 && !writer_big);

	// allocator failure while attaching must not delete the live topic
	quicksand_connection *reader_fail = NULL;
	int64_t success_attach_fail =
			quicksand_connect(&reader_fail, "test", -1, -1, -1, fail_alloc);
	assert(success_attach_fail == -ENOMEM && !reader_fail);
	quicksand_connection *reader_retry = NULL;
	int64_t success_attach_retry =
			quicksand_connect(&reader_retry, "test", -1, -1, -1, NULL);
	assert(success_attach_retry == 0 && reader_retry);
	quicksand_disconnect(&reader_retry, NULL);

	// allocator failure while creating must clean up the half-built topic
	quicksand_connection *writer_alloc_fail = NULL;
	quicksand_delete("allocfail", -1);
	int64_t success_create_fail =
			quicksand_connect(&writer_alloc_fail, "allocfail", -1, 32, 64,
					  fail_alloc);
	assert(success_create_fail == -ENOMEM && !writer_alloc_fail);
	quicksand_connection *allocfail_reader = NULL;
	int64_t allocfail_missing =
			quicksand_connect(&allocfail_reader, "allocfail", -1, -1, -1, NULL);
	assert(allocfail_missing == -ENOENT && !allocfail_reader);
	int64_t success_create_retry =
			quicksand_connect(&writer_alloc_fail, "allocfail", -1, 32, 64,
					  NULL);
	assert(success_create_retry == 0 && writer_alloc_fail);
	quicksand_disconnect(&writer_alloc_fail, NULL);
	quicksand_delete("allocfail", -1);

	// corrupted layout metadata must be rejected before any slot access
	uint64_t saved_message_size = writer->buffer->message_size;
	writer->buffer->message_size = UINT64_MAX;
	assert(quicksand_write(writer, data_write1, sizeof(data_write1)) == -EBADMSG);
	writer->buffer->message_size = saved_message_size;

#ifndef _WIN32
	writer->buffer->message_size = 65; // misaligned slot stride
	assert(quicksand_write(writer, data_write1, 5) == -EBADMSG);
	assert(quicksand_read(reader, data_read1, &size) == -EBADMSG);
	assert(quicksand_connect(&reader_fail, "test", -1, -1, -1, NULL) == -EINVAL);
	writer->buffer->message_size = saved_message_size;
	uint64_t saved_length = writer->buffer->length;
	writer->buffer->length = 1;
	assert(quicksand_write(writer, data_write1, 5) == -EBADMSG);
	assert(quicksand_connect(&reader_fail, "test", -1, -1, -1, NULL) == -EINVAL);
	writer->buffer->length = saved_length;
#endif

	// Latest uses one cursor snapshot, and too-small reads remain retryable.
	assert(quicksand_write(writer, data_write1, 5) == 0);
	assert(quicksand_write(writer, data_write2, 5) == 0);
	size = 1;
	assert(quicksand_read_latest(reader, data_read1, &size) == -EINVAL);
	size = 5;
	assert(quicksand_read(reader, data_read1, &size) == 0);
	assert(memcmp(data_read1, data_write2, 5) == 0);
	assert(quicksand_read_remaining(reader) == 0);

	// delete the topic (unlink shm) and make sure it succeeds
	quicksand_delete("test", -1);
	int64_t success4 = quicksand_connect(&writer_big, "test", -1, 32, 257, NULL);
	assert(success4 == 0 && writer_big);

	quicksand_disconnect(&reader, NULL);
	quicksand_disconnect(&writer, NULL);
	quicksand_disconnect(&writer_big, NULL);
	quicksand_delete("test", -1);
	return 0;
}
