// -------------------------------------------------------------------------
// quicksand.c - implementation of the public API
// -------------------------------------------------------------------------

#define _POSIX_C_SOURCE 200809L // for shm_open, ftruncate, etc.

#include "quicksand.h"
#include "quicksand_crc.h"
#include "quicksand_style.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define QUICKSAND_TIMEOUT 250e6 // nanoseconds

#define DEBUG 1

#if DEBUG
#include <stdio.h>
#endif


// ---------------------------------------------------------------------
// Helper - round a number up to a multiple of 64 (cache line)
// ---------------------------------------------------------------------
static inline i64 round_to_64(i64 v) {
	i64 rem = v & 63;
	return (rem == 0) ? v : (v + (64 - rem));
}

// ---------------------------------------------------------------------
// Helper - round a number up to a power of two
// ---------------------------------------------------------------------
static inline i64 round_to_pow2(i64 v) {
	i64 res = 1;
	while(res < v) {
		if(res > INT64_MAX / 2) {
			return -1;
		}
		res = res << 1;
	}
	return res;
}

static inline int is_power_of_two_u64(u64 value) {
	return value != 0 && (value & (value - 1)) == 0;
}

static i64 checked_add_i64(i64 a, i64 b, i64 *out) {
	if(a < 0 || b < 0) {
		return -EINVAL;
	}
	if(a > INT64_MAX - b) {
		return -EOVERFLOW;
	}
	*out = a + b;
	return 0;
}

static i64 checked_mul_i64(i64 a, i64 b, i64 *out) {
	if(a < 0 || b < 0) {
		return -EINVAL;
	}
	if(a != 0 && b > INT64_MAX / a) {
		return -EOVERFLOW;
	}
	*out = a * b;
	return 0;
}

static i64 build_topic_name(char name_buf[256], const char *topic,
			    i64 topic_length) {
	size_t name_len = 0;

	if(!topic || topic_length < -1) {
		return -EINVAL;
	}

	if(topic_length == -1) {
		name_len = strlen(topic);
	} else {
		name_len = (size_t) topic_length;
	}

	if(name_len == 0) {
		return -EINVAL;
	}
	if(name_len >= sizeof(((quicksand_connection *) 0)->name)) {
		return -ENAMETOOLONG;
	}

	memset(name_buf, 0, 256);
	memcpy(name_buf, topic, name_len);
	name_buf[name_len] = '\0';
	return (i64) name_len;
}

static i64 validate_ringbuffer_layout(const quicksand_ringbuffer *rb,
				      u64 shared_memory_size,
				      i64 expected_length,
				      i64 minimum_message_size) {
	u64 data_offset = (u64) round_to_64((i64) sizeof(quicksand_ringbuffer));
	u64 length;
	u64 message_size;

	if(!rb || shared_memory_size < data_offset) {
		return -EINVAL;
	}

	if(rb->version != QUICKSAND_RING_VERSION) {
		return -EPROTONOSUPPORT;
	}

	length = rb->length;
	message_size = rb->message_size;
	if(length < 2 || !is_power_of_two_u64(length)
	   || message_size < 64 || (message_size & 63)) {
		return -EINVAL;
	}
	if(expected_length > 0 && length != (u64) expected_length) {
		return -EINVAL;
	}
	if(minimum_message_size > 0 && message_size < (u64) minimum_message_size) {
		return -EINVAL;
	}
	if(message_size > shared_memory_size - data_offset) {
		return -EINVAL;
	}
	if(length > (shared_memory_size - data_offset) / message_size) {
		return -EINVAL;
	}
	return 0;
}

// Cleanup is explicitly best-effort: init paths must unwind cleanly, but a
// failed close/munmap/shm_unlink must not mask the original result.
static inline void best_effort_close(int fd) {
	if(fd >= 0) {
		int rc = close(fd);
		(void) rc;
	}
}

static inline void best_effort_munmap(void *addr, size_t size) {
	if(addr && addr != MAP_FAILED && size > 0) {
		int rc = munmap(addr, size);
		(void) rc;
	}
}

static inline void best_effort_shm_unlink(const char *name) {
	if(name && name[0] != '\0') {
		int rc = shm_unlink(name);
		(void) rc;
	}
}

// ---------------------------------------------------------------------
// Helper - copy the topic name (null terminated) into the 256-word
//           array that lives inside quicksand_connection.
// ---------------------------------------------------------------------
static inline void copy_topic_to_name(quicksand_connection *c, const char *topic,
				      i64 topic_len) {
	memset(c->name, 0, sizeof(c->name));
	size_t copy_len = (size_t) topic_len;
	if(copy_len >= sizeof(c->name)) {
		copy_len = sizeof(c->name) - 1; // truncate
	}

	memcpy(c->name, topic, copy_len);
}

// ---------------------------------------------------------------------
// quicksand_connect - create or attach to an existing shm segment
// ---------------------------------------------------------------------
i64 quicksand_connect(quicksand_connection **out, char *topic,
		      i64 topic_length, i64 message_size,
		      i64 message_rate, void *alloc) {
	i64 rc;
	i64 topic_name_len;
	quicksand_connection *connection = NULL;

	// -----------------------------------------------------------------
	// Parameter validation
	// -----------------------------------------------------------------
	if(!topic || topic_length > 255) {
		return -EINVAL;
	}

	// ---------------------------------------------------------------
	// Allocation - fall back to malloc() if the caller gave us NULL.
	// ---------------------------------------------------------------
	void *(*allocate)(size_t) = malloc;
	if(alloc) {
		allocate = (void *(*) (size_t) ) alloc;
	}

	// ---------------------------------------------------------------
	// Build a null-terminated C string from the caller-supplied name.
	// ---------------------------------------------------------------
	char name_buf[256] = {0};
	topic_name_len = build_topic_name(name_buf, topic, topic_length);
	if(topic_name_len < 0) {
		return topic_name_len;
	}

	// ---------------------------------------------------------------
	// If message_size == 0 || message_rate == 0 we are only *connecting*
	// to an already existing segment.
	// ---------------------------------------------------------------
	if(message_size <= 0 || message_rate <= 0) {
		int fd = shm_open(name_buf, O_RDWR, 0);
		u64 shared_memory_size = 0;

		if(fd == -1) {
			return -errno; // Preserve errors such as EACCES and EMFILE.
		}

		struct stat sb;
		if(fstat(fd, &sb) < 0) {
			rc = -errno;
			best_effort_close(fd);
			return rc;
		}
		if(sb.st_size < (off_t) sizeof(quicksand_ringbuffer)) {
			best_effort_close(fd);
			return -EINVAL; // too small
		}
		shared_memory_size = (u64) sb.st_size;

		void *addr = mmap(NULL, (size_t) shared_memory_size,
				  PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if(addr == MAP_FAILED) {
			rc = -errno;
			best_effort_close(fd);
			return rc;
		}

		quicksand_ringbuffer *rb = (quicksand_ringbuffer *) addr;
		rc = validate_ringbuffer_layout(rb, shared_memory_size, 0, 64);
		if(rc < 0) {
			best_effort_munmap(addr, (size_t) shared_memory_size);
			best_effort_close(fd);
			return rc;
		}

		if(!out) {
			best_effort_munmap(addr, (size_t) shared_memory_size);
			best_effort_close(fd);
			return 0;
		}

		connection = *out;
		if(!connection) {
			connection = allocate(sizeof(quicksand_connection));
		}
		if(!connection) {
			best_effort_munmap(addr, (size_t) shared_memory_size);
			best_effort_close(fd);
			return -ENOMEM;
		}

		memset(connection, 0, sizeof(*connection));
		connection->read_stamp = quicksand_now();
		connection->read_index = 0;
		connection->shared_memory_handle = (u64) fd;
		connection->shared_memory_size = shared_memory_size;
		connection->buffer = rb;
		copy_topic_to_name(connection, name_buf, topic_name_len);
		*out = connection;
		return 0;
	}

	// ---------------------------------------------------------------
	// Otherwise we need to create a new ring buffer (or connect to an
	// already existing one).  First try O_EXCL to see if it already
	// exists - if it does, fall back to plain O_RDWR.
	// ---------------------------------------------------------------

	// ---------------------------------------------------------------
	// Compute the total size of the segment.
	//
	//   data_offset = round_up(sizeof(quicksand_ringbuffer)) to 64-byte
	//   padded_message = round_up(20 + message_size) // preamble + CRC
	//   payload_area  = padded_message * message_rate   // 1-second worth
	//   shm_size      = data_offset + payload_area
	// ---------------------------------------------------------------
	i64 data_offset = round_to_64((i64) sizeof(quicksand_ringbuffer));
	i64 padded_unaligned = 0;
	i64 padded_msg = 0;
	i64 ring_length = 0;
	i64 payload_area = 0;
	i64 shm_size = 0;

	// reserve enough space for 1e9 ns (1 second) of messages.
	// padded = [write_timestamp] + [message_len] + [message] + [CRC32C]
	rc = checked_add_i64(message_size, 20, &padded_unaligned);
	if(rc < 0) {
		return rc;
	}
	if(padded_unaligned > INT64_MAX - 63) {
		return -EOVERFLOW;
	}
	padded_msg = round_to_64(padded_unaligned);
	if(padded_msg < 64) {
		return -EOVERFLOW;
	}
	// Keep at least one readable slot and one writable slot.
	ring_length = round_to_pow2(message_rate < 2 ? 2 : message_rate);
	if(ring_length <= 0) {
		return -EOVERFLOW;
	}
	rc = checked_mul_i64(padded_msg, ring_length, &payload_area);
	if(rc < 0) {
		return rc;
	}
	rc = checked_add_i64(data_offset, payload_area, &shm_size);
	if(rc < 0) {
		return rc;
	}

	i64 mapped_size = shm_size;
	int already_exists = 0;
	int fd = shm_open(name_buf, O_EXCL | O_CREAT | O_RDWR,
			  S_IRUSR | S_IWUSR);
	if(fd == -1 && errno == EEXIST) {
		struct stat sb;

		fd = shm_open(name_buf, O_RDWR, 0);
		if(fd == -1) {
			return -errno;
		}
		if(fstat(fd, &sb) < 0) {
			rc = -errno;
			best_effort_close(fd);
			return rc;
		}
		if(sb.st_size < (off_t) sizeof(quicksand_ringbuffer)) {
			best_effort_close(fd);
			return -EINVAL; // too small
		}
		mapped_size = (i64) sb.st_size;
		already_exists = 1;
	}
	if(fd == -1) {
		return -errno; // cannot create/open segment
	}

	// Resize the shm object to the required size
	if(!already_exists) {
		if(ftruncate(fd, (off_t) shm_size) == -1) {
			rc = -errno;
			best_effort_close(fd);
			best_effort_shm_unlink(name_buf);
			return rc;
		}
	}

	void *addr = mmap(NULL, (size_t) mapped_size, PROT_READ | PROT_WRITE,
			  MAP_SHARED, fd, 0);
	if(addr == MAP_FAILED) {
		rc = -errno;
		best_effort_close(fd);
		if(!already_exists) {
			best_effort_shm_unlink(name_buf);
		}
		return rc;
	}

	// -----------------------------------------------------------------
	// Initialise the meta-data.  The original code stored the length as
	// a power-of-two (so that modulo can be replaced by & (len-1)).
	// -----------------------------------------------------------------
	quicksand_ringbuffer *rb = (quicksand_ringbuffer *) addr;
	if(!already_exists) {
		rb->length = ring_length;
		rb->message_size = padded_msg;
		rb->version = QUICKSAND_RING_VERSION;
		atomic_store_explicit(&rb->reserve, 0, memory_order_relaxed);
		atomic_store_explicit(&rb->index, 0, memory_order_relaxed);
		atomic_store_explicit(&rb->updatestamp, 0, memory_order_relaxed);
		atomic_store_explicit(&rb->locked, 0, memory_order_relaxed);
	}
	rc = validate_ringbuffer_layout(rb, (u64) mapped_size, ring_length,
					padded_msg);
	if(rc == 0 && mapped_size != shm_size) {
		rc = -EINVAL;
	}
	if(rc < 0) {
		best_effort_munmap(addr, (size_t) mapped_size);
		best_effort_close(fd);
		if(!already_exists) {
			best_effort_shm_unlink(name_buf);
		}
		return rc;
	}

	if(!out) {
		best_effort_munmap(addr, (size_t) mapped_size);
		best_effort_close(fd);
		return 0;
	}

	connection = *out;
	if(!connection) {
		connection = allocate(sizeof(quicksand_connection));
	}
	if(!connection) {
		best_effort_munmap(addr, (size_t) mapped_size);
		best_effort_close(fd);
		if(!already_exists) {
			best_effort_shm_unlink(name_buf);
		}
		return -ENOMEM;
	}

	memset(connection, 0, sizeof(*connection));
	connection->read_stamp = quicksand_now();
	connection->read_index = 0;
	connection->shared_memory_handle = (u64) fd;
	connection->shared_memory_size = (u64) shm_size;
	connection->buffer = rb;
	copy_topic_to_name(connection, name_buf, topic_name_len);
	*out = connection;

	return 0;
}

// ---------------------------------------------------------------------
// quicksand_disconnect - undo everything done in quicksand_connect
// ---------------------------------------------------------------------
void quicksand_disconnect(quicksand_connection **c, void *dealloc) {
	if(!c || !(*c)) {
		return;
	}

	if((*c)->buffer && (*c)->shared_memory_size > 0) {
		// Unmap the segment first
		best_effort_munmap((void *) (*c)->buffer,
				   (size_t) (*c)->shared_memory_size);
		best_effort_close((int) (*c)->shared_memory_handle);
		// shm_unlink((char*)(*c)->name); // removes the buffer for future
		// use quicksand_delete(name, namelen) instead
	}

	// free() the struct that the caller allocated (the pointer itself)
	// if they passed a custom deallocator, use it.
	void (*deallocate)(void *) = free;
	if(dealloc) {
		deallocate = (void (*)(void *)) dealloc;
	}
	deallocate(*c);
	*c = NULL;
}

void quicksand_delete(char *topic, i64 topic_length) {
	char name_buf[256] = {0};
	i64 topic_name_len = build_topic_name(name_buf, topic, topic_length);
	if(topic_name_len < 0) {
		return;
	}
	best_effort_shm_unlink(name_buf);
}

// ---------------------------------------------------------------------
// internal - attempt to un-lock a stalled ringbuffer.
// ---------------------------------------------------------------------
static i64 _quicksand_unlock(quicksand_ringbuffer *ring, u64 locktime) {
	u64 now = quicksand_now();
	if(!ring) {
		return -1;
	}

	// only if sufficient time passed, attempt unlock.
	if(quicksand_ns(now, locktime) <= QUICKSAND_TIMEOUT) {
		return -2;
	}

	// Return success if ring is already alive.
	if(!ring->locked) {
		return 0;
	}

	u64 current = locktime;
	// attempt to update locktime
	if(!atomic_compare_exchange_strong_explicit(&ring->locked, &current,
						    now, memory_order_relaxed, memory_order_relaxed)) {
		return -2;
	}
	atomic_store_explicit(&ring->updatestamp, now, memory_order_relaxed);
	ring->reserve = ring->index;
	atomic_store_explicit(&ring->locked, 0, memory_order_release);
	return 0;
}


// ---------------------------------------------------------------------
// quicksand_write - put a new payload into the ring buffer
// ---------------------------------------------------------------------
i64 quicksand_write(quicksand_connection *c, u8 *msg, i64 msg_len) {
	u64 start_time = quicksand_now();
	u64 ring_length = 0;
	u64 slot_size = 0;
	u64 data_offset = 0;
	u64 slot = 0;
	i64 rc = 0;
	if(!c || !msg) {
		return -EINVAL;
	}
	quicksand_ringbuffer *rb = c->buffer;
	if(!rb) {
		return -EPIPE; // uninitialised
	}
	rc = validate_ringbuffer_layout(rb, c->shared_memory_size, 0, 64);
	if(rc < 0) {
		return -EBADMSG;
	}
	ring_length = rb->length;
	slot_size = rb->message_size;
	data_offset = (u64) round_to_64((i64) sizeof(quicksand_ringbuffer));

	// attempt unlock
	u64 locktime = atomic_load_explicit(&rb->locked, memory_order_relaxed);
	if(locktime != 0) {
		_quicksand_unlock(rb, locktime);
		return -ETIMEDOUT;
	}


	if(msg_len < 0 || (u64) msg_len > slot_size - 20) {
		return -EMSGSIZE; // message does not fit
	}

	// -----------------------------------------------------------------
	// 1. Reserve a slot (atomic fetch-add)
	// -----------------------------------------------------------------
	u64 my_reserve = atomic_load_explicit(&rb->reserve, memory_order_relaxed);
	while(!atomic_compare_exchange_weak_explicit(&rb->reserve, &my_reserve,
						     my_reserve + 1, memory_order_relaxed, memory_order_relaxed)) {
		if(quicksand_ns(quicksand_now(), start_time) > QUICKSAND_TIMEOUT / 2) {
			return -ETIMEDOUT;
		}
	}

	// -----------------------------------------------------------------
	// 2. Block until reserve < 50% away from index
	// -----------------------------------------------------------------
	while(my_reserve - atomic_load_explicit(&rb->index, memory_order_relaxed)
	      >= ring_length / 2) {
		if(quicksand_ns(quicksand_now(), start_time) > QUICKSAND_TIMEOUT / 2) {
			atomic_store_explicit(&rb->locked, quicksand_now(), memory_order_relaxed);
			return -ETIMEDOUT;
		}
	}

	// -----------------------------------------------------------------
	// 3. Write the message
	// -----------------------------------------------------------------
	slot = my_reserve & (ring_length - 1);
	u8 *base = (u8 *) rb;
	u8 *slot_ptr = base + data_offset + slot * slot_size;

	u8 preamble[16];
	u64 timestamp = quicksand_now();
	memcpy(preamble, &timestamp, sizeof(timestamp));
	memcpy(preamble + 8, &msg_len, sizeof(msg_len));
	uint32_t crc = fast_memcpy_crc32c(slot_ptr, preamble, sizeof(preamble), 0);
	crc = fast_memcpy_crc32c(slot_ptr + 16, msg, (size_t) msg_len, crc);
	memcpy(slot_ptr + 16 + msg_len, &crc, sizeof(crc));

	// -----------------------------------------------------------------
	// 4. Wait to advance index
	// -----------------------------------------------------------------
	while(my_reserve != atomic_load_explicit(&rb->index, memory_order_relaxed)) {
		if(quicksand_ns(quicksand_now(), start_time) > QUICKSAND_TIMEOUT / 2) {
			atomic_store_explicit(&rb->locked, start_time, memory_order_release);
			return -ETIMEDOUT;
		}
	}

	atomic_store_explicit(&rb->updatestamp, quicksand_now(), memory_order_relaxed);
	// Recovery may have overtaken us after the wait; never roll index back.
	if(!atomic_compare_exchange_strong_explicit(&rb->index, &my_reserve,
						    my_reserve + 1, memory_order_release, memory_order_relaxed)) {
		return -ETIMEDOUT;
	}

	return 0;
}

// ---------------------------------------------------------------------
// quicksand_read - fetch the next available payload, if any
// ---------------------------------------------------------------------
i64 quicksand_read(quicksand_connection *c, u8 *msg, i64 *msg_len) {
	u64 ring_length = 0;
	u64 slot_size = 0;
	u64 data_offset = 0;
	i64 rc = 0;
	if(!c || !msg || !msg_len) {
		return -EINVAL;
	}
	quicksand_ringbuffer *rb = c->buffer;

	if(!rb) {
		return -EPIPE; // not initialized
	}
	rc = validate_ringbuffer_layout(rb, c->shared_memory_size, 0, 64);
	if(rc < 0) {
		return -EBADMSG;
	}
	ring_length = rb->length;
	slot_size = rb->message_size;
	data_offset = (u64) round_to_64((i64) sizeof(quicksand_ringbuffer));

	// -----------------------------------------------------------------
	// 1. Load the current write index (the slot that *has* been
	//    committed). Keep this polling load relaxed by design.
	// -----------------------------------------------------------------
	u64 write_cursor = atomic_load_explicit(&rb->index,
						memory_order_relaxed);

	// -----------------------------------------------------------------
	// 2. Did we already consume this slot?  The read index is stored in
	//    the connection object.
	// -----------------------------------------------------------------
	// if(write_cursor - c->read_index > (u64) 1e18) {  // Should not happen, reader>writer
	// 	c->read_index = write_cursor;
	// }
	u64 now = quicksand_now();
	if(c->read_index == write_cursor) {
		// printf("read index at limit\n");
		// No new message - consumer is caught up
		c->read_stamp = now;
		return -1; // "0 messages read"
	}

	// -----------------------------------------------------------------
	// 3. The writer may have advanced *many* slots ahead.  If the distance
	//    is larger than half the ring we clamp the "oldest readable" slot
	//    to (write_cursor-1).
	// -----------------------------------------------------------------
	u64 distance = write_cursor - c->read_index;
	f64 time_delta = quicksand_ns(rb->updatestamp, c->read_stamp);
	if(distance > (ring_length / 2)
	   || (time_delta > QUICKSAND_TIMEOUT && write_cursor > c->read_index)) {
		c->read_index = write_cursor - 1; // skip stale data
	}

	// -----------------------------------------------------------------
	// 4. Compute slot location and update read index
	// -----------------------------------------------------------------
	u64 slot = c->read_index & (ring_length - 1);
	u8 *base = (u8 *) rb;
	u8 *slot_ptr = base + data_offset + slot * slot_size;

	// -----------------------------------------------------------------
	// 5. Advance our local read pointer so the next call reads the next slot.
	// -----------------------------------------------------------------
	c->read_index = c->read_index + 1;
	c->read_stamp = now;

	// -----------------------------------------------------------------
	// 6. Read the timestamp and size that the writer stored at front
	// -----------------------------------------------------------------
	u64 preamble[2];
	uint32_t crc = fast_memcpy_crc32c((u8 *) preamble, slot_ptr, sizeof(preamble), 0);
	i64 payload_len = (i64) preamble[1];
	if(payload_len < 0 || (u64) payload_len > slot_size - 20) {
		// Corrupted size: consume the message without accessing its payload.
		return -EBADMSG;
	}

	// -----------------------------------------------------------------
	// 7. Copy message length to the buffer supplied by the caller.
	// ----------------------------------------------------------------
	if(*msg_len < payload_len) {
		--c->read_index; // Keep the selected message available for retry.
		return -EINVAL;	 // too short
	}

	crc = fast_memcpy_crc32c(msg, slot_ptr + 16, (size_t) payload_len, crc);
	uint32_t stored_crc;
	memcpy(&stored_crc, slot_ptr + 16 + payload_len, sizeof(stored_crc));
	if(crc != stored_crc) {
		return -EBADMSG;
	}
	*msg_len = payload_len; // tell the caller how many bytes we wrote

	// Return the number of messages still pending after we consumed one.
	return (i64) (write_cursor - c->read_index);
}
