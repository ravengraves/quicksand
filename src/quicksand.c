// -------------------------------------------------------------------------
// quicksand.c - implementation of the public API
// -------------------------------------------------------------------------
//
// The implementation follows the algorithm that was used in the old
// "ravenring" code.  The only differences are:
//   •  the names of the structs and functions,
//   •  the way we obtain the allocator (malloc() by default),
//   •  a small amount of defensive checking for the new API.
//
// Everything else (slot reservation, cursor checking, timeout handling,
// padding to cache-line boundaries, storing the payload size in the first
// eight bytes of a slot) is kept 100% compatible.
// -------------------------------------------------------------------------

#define _POSIX_C_SOURCE 200809L // for shm_open, ftruncate, etc.

#include "quicksand.h"
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


#if defined(__GNUC__) || defined(__clang__)
#define RESTRICT __restrict__
#elif defined(_MSC_VER)
#define RESTRICT __restrict
#else
#define RESTRICT restrict
#endif

/* Fixed-size transfers through locals are valid for arbitrary byte alignment
 * and object types. Keep each loaded value explicit for future checksum work.
 */
static inline void fast_memcpy(unsigned char *RESTRICT d,
			       const unsigned char *RESTRICT s, size_t n) {
	uint64_t w0, w1, w2, w3, w4, w5, w6, w7;
	uint32_t w8;
	uint16_t w9;
	size_t prefix;

	/* Amortize source cache-line alignment on copies of at least 4 KiB.
	 * The destination may have a different alignment and remains supported. */
	if(n >= 4096) {
		prefix = (size_t) (-(uintptr_t) s & 63);
		n -= prefix;
		if(prefix >= 32) {
			memcpy(&w0, s + 0, sizeof(w0));
			memcpy(d + 0, &w0, sizeof(w0));
			memcpy(&w1, s + 8, sizeof(w1));
			memcpy(d + 8, &w1, sizeof(w1));
			memcpy(&w2, s + 16, sizeof(w2));
			memcpy(d + 16, &w2, sizeof(w2));
			memcpy(&w3, s + 24, sizeof(w3));
			memcpy(d + 24, &w3, sizeof(w3));
			d += 32;
			s += 32;
			prefix -= 32;
		}
		if(prefix >= 16) {
			memcpy(&w0, s + 0, sizeof(w0));
			memcpy(d + 0, &w0, sizeof(w0));
			memcpy(&w1, s + 8, sizeof(w1));
			memcpy(d + 8, &w1, sizeof(w1));
			d += 16;
			s += 16;
			prefix -= 16;
		}
		if(prefix >= 8) {
			memcpy(&w0, s + 0, sizeof(w0));
			memcpy(d + 0, &w0, sizeof(w0));
			d += 8;
			s += 8;
			prefix -= 8;
		}
		if(prefix >= 4) {
			memcpy(&w8, s, sizeof(w8));
			memcpy(d, &w8, sizeof(w8));
			d += 4;
			s += 4;
			prefix -= 4;
		}
		if(prefix >= 2) {
			memcpy(&w9, s, sizeof(w9));
			memcpy(d, &w9, sizeof(w9));
			d += 2;
			s += 2;
			prefix -= 2;
		}
		if(prefix) {
			*d++ = *s++;
		}
	}
	while(n >= 64) {
		memcpy(&w0, s + 0, sizeof(w0));
		memcpy(d + 0, &w0, sizeof(w0));
		memcpy(&w1, s + 8, sizeof(w1));
		memcpy(d + 8, &w1, sizeof(w1));
		memcpy(&w2, s + 16, sizeof(w2));
		memcpy(d + 16, &w2, sizeof(w2));
		memcpy(&w3, s + 24, sizeof(w3));
		memcpy(d + 24, &w3, sizeof(w3));
		memcpy(&w4, s + 32, sizeof(w4));
		memcpy(d + 32, &w4, sizeof(w4));
		memcpy(&w5, s + 40, sizeof(w5));
		memcpy(d + 40, &w5, sizeof(w5));
		memcpy(&w6, s + 48, sizeof(w6));
		memcpy(d + 48, &w6, sizeof(w6));
		memcpy(&w7, s + 56, sizeof(w7));
		memcpy(d + 56, &w7, sizeof(w7));
		d += 64;
		s += 64;
		n -= 64;
	}
	if(!n) {
		return;
	}

	/* Bounded remainder: no reads or writes past the requested length. */
	if(n >= 32) {
		memcpy(&w0, s + 0, sizeof(w0));
		memcpy(d + 0, &w0, sizeof(w0));
		memcpy(&w1, s + 8, sizeof(w1));
		memcpy(d + 8, &w1, sizeof(w1));
		memcpy(&w2, s + 16, sizeof(w2));
		memcpy(d + 16, &w2, sizeof(w2));
		memcpy(&w3, s + 24, sizeof(w3));
		memcpy(d + 24, &w3, sizeof(w3));
		d += 32;
		s += 32;
		n -= 32;
	}
	if(n >= 16) {
		memcpy(&w0, s + 0, sizeof(w0));
		memcpy(d + 0, &w0, sizeof(w0));
		memcpy(&w1, s + 8, sizeof(w1));
		memcpy(d + 8, &w1, sizeof(w1));
		d += 16;
		s += 16;
		n -= 16;
	}
	if(n >= 8) {
		memcpy(&w0, s + 0, sizeof(w0));
		memcpy(d + 0, &w0, sizeof(w0));
		d += 8;
		s += 8;
		n -= 8;
	}
	if(n >= 4) {
		memcpy(&w8, s, sizeof(w8));
		memcpy(d, &w8, sizeof(w8));
		d += 4;
		s += 4;
		n -= 4;
	}
	if(n >= 2) {
		memcpy(&w9, s, sizeof(w9));
		memcpy(d, &w9, sizeof(w9));
		d += 2;
		s += 2;
		n -= 2;
	}
	if(n) {
		*d = *s;
	}
}

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

	length = rb->length;
	message_size = rb->message_size;
	if(length < 2 || !is_power_of_two_u64(length)
	   || message_size < 16 || (message_size & 63)) {
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
		rc = validate_ringbuffer_layout(rb, shared_memory_size, 0, 16);
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
	//   padded_message = round_up(8 + message_size)   // we keep the 8-byte
	//           // header that stores the actual payload size
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
	// padded = [write_timestamp] + [message_len] + [message]
	rc = checked_add_i64(message_size, 16, &padded_unaligned);
	if(rc < 0) {
		return rc;
	}
	if(padded_unaligned > INT64_MAX - 63) {
		return -EOVERFLOW;
	}
	padded_msg = round_to_64(padded_unaligned);
	if(padded_msg < 16) {
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
		if(sb.st_size != shm_size) { // Abort if size does not match
			best_effort_close(fd);
			return -EINVAL; // too small
		}
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

	void *addr = mmap(NULL, (size_t) shm_size, PROT_READ | PROT_WRITE,
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
		atomic_store_explicit(&rb->reserve, 0, memory_order_relaxed);
		atomic_store_explicit(&rb->index, 0, memory_order_relaxed);
		atomic_store_explicit(&rb->updatestamp, 0, memory_order_relaxed);
		atomic_store_explicit(&rb->locked, 0, memory_order_relaxed);
	}
	rc = validate_ringbuffer_layout(rb, (u64) shm_size, ring_length,
					padded_msg);
	if(rc < 0) {
		best_effort_munmap(addr, (size_t) shm_size);
		best_effort_close(fd);
		if(!already_exists) {
			best_effort_shm_unlink(name_buf);
		}
		return rc;
	}

	if(!out) {
		best_effort_munmap(addr, (size_t) shm_size);
		best_effort_close(fd);
		return 0;
	}

	connection = *out;
	if(!connection) {
		connection = allocate(sizeof(quicksand_connection));
	}
	if(!connection) {
		best_effort_munmap(addr, (size_t) shm_size);
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
	rc = validate_ringbuffer_layout(rb, c->shared_memory_size, 0, 16);
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


	if(msg_len < 0 || (u64) msg_len > slot_size - 16) {
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

	*((u64 *) slot_ptr) = quicksand_now();
	*((i64 *) (slot_ptr + 8)) = msg_len;
	fast_memcpy(slot_ptr + 16, msg, (u64) msg_len);

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
	atomic_store_explicit(&rb->index, my_reserve + 1, memory_order_release);

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
	rc = validate_ringbuffer_layout(rb, c->shared_memory_size, 0, 16);
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
	// u64 payload_stamp = *((u64 *) slot_ptr); // unused
	i64 payload_len = *((i64 *) (slot_ptr + 8));
	if(payload_len < 0 || (u64) payload_len > slot_size - 16) {
		// Corrupted size - treat as no-data
		return -EBADMSG;
	}

	// -----------------------------------------------------------------
	// 7. Copy to the buffer supplied by the caller.
	// ----------------------------------------------------------------
	if(*msg_len < payload_len) {
		--c->read_index; // Keep the selected message available for retry.
		return -EINVAL;	 // too short
	}

	fast_memcpy(msg, slot_ptr + 16, payload_len);
	*msg_len = payload_len; // tell the caller how many bytes we wrote

	// Return the number of messages still pending after we consumed one.
	return (i64) (write_cursor - c->read_index);
}
