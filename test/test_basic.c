#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>

#include "quicksand.h"

static void *fail_alloc(size_t size) {
	(void) size;
	return NULL;
}

int main(void) {
	quicksand_connection *writer = NULL;
	quicksand_connection *reader = NULL;
	quicksand_delete("test", -1);
	int64_t success1 = quicksand_connect(&writer, "test", -1, 32, 100, NULL);
	int64_t success2 = quicksand_connect(&reader, "test", -1, -1, -1, NULL);

	assert(success1 == 0 && writer);
	assert(success2 == 0 && reader);

	uint8_t data_write1[5] = {1, 2, 3, 4, 5};
	quicksand_write(writer, data_write1, sizeof(data_write1));
	uint8_t data_write2[5] = {6, 7, 8, 9, 10};
	quicksand_write(writer, data_write2, sizeof(data_write2));
	int64_t size = 5;
	uint8_t data_read1[size], data_read2[size];
	int64_t read_ret1 = quicksand_read(reader, data_read1, &size);
	assert(read_ret1 >= 0);
	int64_t read_ret2 = quicksand_read(reader, data_read2, &size);
	assert(read_ret2 == 0);

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
