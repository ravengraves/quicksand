/**
 * @file quicksand.h
 * @brief Public interface for Quicksand, a lock-free shared-memory ringbuffer
 *  communication library
 *
 * This header defines the data structures, API functions, and macros used to
 * create, connect to, read from and write to a lock-free ring buffer that is
 * backed by a shared memory segment.  The implementation is written in
 * C but can be used from C++ code thanks to the extern "C" guard.
 */

#ifndef QUICKSAND_H
#define QUICKSAND_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdatomic.h>
#include <stdint.h>

/** Size of a cache line on most modern CPUs (in bytes). */
#define CACHE_LINE_SIZE 64

/** Shared-memory protocol version; incompatible topics must be recreated. */
#define QUICKSAND_RING_VERSION 1u

/** @brief Ring buffer data structure.
 *
 *  The ring buffer is placed in shared memory immediately followed by the
 *  actual message payload array (`char data[]`).  All indices are modulo
 *  `length`.  The layout is padded to avoid false sharing between threads.
 */
typedef struct {
	/** Number of slots in the buffer. */
	uint64_t length;

	/** Slot stride in bytes: round_up(payload capacity + 20, 64). */
	uint64_t message_size;

	/** Ring protocol version, stored in formerly unused header padding. */
	uint32_t version;

	/** Padding to align the next field to a cache line boundary. */
	char pad1[CACHE_LINE_SIZE - 2 * sizeof(int64_t) - sizeof(uint32_t)];

	/** Writer reserve index (atomic).  Writers increment this value before
     *  writing a slot to claim it. */
	volatile _Atomic(uint64_t) reserve;

	/** Padding to align the next field to a cache line boundary. */
	char pad2[CACHE_LINE_SIZE - sizeof(uint64_t)];

	/** Current head index of the ring buffer (atomic).  Readers consume
     *  slots up to this value. */
	volatile _Atomic(uint64_t) index;

	/** Last update timestamp (atomic). */
	volatile _Atomic(uint64_t) updatestamp;

	/** Write timeout stamp (atomic). */
	volatile _Atomic(uint64_t) locked;

	/** Padding to align the next field to a cache line boundary. */
	char pad3[CACHE_LINE_SIZE - 3 * sizeof(uint64_t)];
} quicksand_ringbuffer;

/** @brief Reader/Writer connection information. */
typedef struct {
	/** Last read timestamp. */
	uint64_t read_stamp;

	/** Last read index. */
	uint64_t read_index;

	/** OS shared-memory handle. */
	uint64_t shared_memory_handle;

	/** Size of the shared memory segment. */
	uint64_t shared_memory_size;

	/** Mapped ring buffer address. */
	quicksand_ringbuffer *buffer;

	/** Shared memory name. */
	uint8_t name[256];
} quicksand_connection;

/* -------------------------------------------------------------------------- */
/* Core reading/writing                                                       */
/* -------------------------------------------------------------------------- */

/** @brief Connect to a shared-memory ring buffer.
 *
 *  This function creates or connects to a POSIX shared memory segment that
 *  contains a Quicksand ring buffer.  The caller receives a pointer to a
 *  @c quicksand_connection structure that can be used for subsequent read
 *  and write operations.
 *
 *  @param connection   (OUT) Pointer to a @c quicksand_connection* that will
 *                      be allocated and initialized on success.  Pass
 *                      @c NULL to receive an error code without allocating
 *                      a connection object.
 *  @param topic        Shared memory segment name.
 *  @param topic_length Length of the name in bytes (excluding the terminating
 *                      null); pass @c -1 to use @c strlen().
 *  @param message_size Maximum size per message in bytes; pass @c -1 to use
 *                      the default size specified by the buffer.
 *  @param message_rate Maximum number of messages per second; pass @c -1 to
 *                      use the default rate specified by the buffer. Creation rounds
 *                      capacity up to a power of two, with at least two slots.
 *  @param alloc        Custom allocator that follows @c malloc(size_t)
 *                      semantics, or @c NULL to use the system allocator.
 *
 *  @return 0 on success, or a negative error code on failure. Incompatible
 *          ring versions return -EPROTONOSUPPORT. Stop old processes and
 *          recreate topics when upgrading; old binaries may accept this format.
 */
int64_t quicksand_connect(quicksand_connection **connection, char *topic,
			  int64_t topic_length, int64_t message_size,
			  int64_t message_rate, void *alloc);

/** @brief Disconnect from a ring buffer and free the connection memory.
 *
 *  The function releases the shared memory mapping and frees the
 *  @c quicksand_connection structure.  A custom deallocator can be supplied
 *  that follows @c free(void*) semantics.
 *
 *  @param connection Pointer to the connection to disconnect.  The pointer
 *                    will be set to @c NULL after the call.
 *  @param dealloc    Custom deallocator, or @c NULL to use the system
 *                    deallocator.
 */
void quicksand_disconnect(quicksand_connection **connection, void *dealloc);

/** @brief Delete a topic, freeing it for recreation.
 *
 *  This removes the shared memory segment identified by @p topic, making it
 *  possible to create a fresh buffer with the same name later on.  Existing
 *  connections retain their old mapping until disconnected; a newly created
 *  topic with the same name is separate.
 *
 *  @param topic        Shared memory segment name.
 *  @param topic_length Length of the name in bytes (excluding the null);
 *                      pass @c -1 to use @c strlen().
 */
void quicksand_delete(char *topic, int64_t topic_length);

/** @brief Write a message to the ring buffer.
 *
 *  The caller supplies a pointer to the data and its size.  The function
 *  copies the data into the next available slot in the shared memory
 *  buffer.
 *
 *  @param connection   Initialized @c quicksand_connection.
 *  @param message      Pointer to the message data to write.
 *  @param message_size Size of the data to write in bytes.
 *
 *  @return 0 on success, or a negative error code on failure. -ETIMEDOUT
 *          reports a reservation overtaken by timeout recovery before
 *          publication.
 */
int64_t quicksand_write(quicksand_connection *connection, uint8_t *message,
			int64_t message_size);

/** @brief Read a message from the ring buffer.
 *
 *  The function attempts to read the next available message.  If a message
 *  is available, it copies up to @p *message_size bytes into the provided
 *  buffer and updates @p *message_size with the number of bytes actually
 *  read.
 *  If the writer is > 50% of the ring length past the reader, or > timeout
 *  has elapsed since the last read, this will only read the last message.
 *  Empty polling is relaxed; available data is acquired through the published
 *  index before selecting and copying a slot.
 *
 *  @param connection   Initialized @c quicksand_connection.
 *  @param message      Buffer to receive the message.
 *  @param message_size (IN/OUT) Maximum size to read; overwritten with the
 *                      number of bytes actually read.
 *
 *  @return Number of messages remaining in the buffer, or -1 if no message
 *          was read, or a negative errno on error. A buffer that is too small
 *          returns -EINVAL without consuming the selected message; retry with
 *          a larger buffer, subject to the usual overwrite/lag policy.
 *          A corrupt length or CRC-32C mismatch returns -EBADMSG and consumes
 *          the selected message. The caller's length is unchanged on failure;
 *          discard any output bytes. CRC covers the preamble and all payload
 *          bytes; it provides error detection, not synchronization or guaranteed
 *          detection of concurrent overwrites.
 */
int64_t quicksand_read(quicksand_connection *connection, uint8_t *message,
		       int64_t *message_size);

/* -------------------------------------------------------------------------- */
/* Timing functions                                                           */
/* -------------------------------------------------------------------------- */

/** @brief Monotonic timestamp counter (RDTSCP on x86_64).
 *
 *  @return Current CPU timestamp counter.
 */
uint64_t quicksand_now(void);

/** @brief Compute elapsed nanoseconds between two timestamps.
 *
 *  Calibrates the calling thread's timer on first use.
 *
 *  @param final_timestamp   Timestamp at the end of the interval.
 *  @param initial_timestamp Timestamp at the beginning of the interval.
 *
 *  @return Number of nanoseconds elapsed between the two timestamps.
 */
double quicksand_ns(uint64_t final_timestamp, uint64_t initial_timestamp);

/** @brief Calibrate the nanosecond timer.
 *
 *  The function sleeps for the specified amount of nanoseconds while
 *  measuring the CPU ticks to determine the conversion factor from ticks
 *  to nanoseconds. Calibration applies only to the calling thread.
 *
 *  @param nanoseconds Amount of time to sleep during calibration.
 *
 *  @return Number of nanoseconds per tick.
 */
double quicksand_ns_calibrate(double nanoseconds);

/** @brief Busy-wait or sleep for the specified number of nanoseconds.
 *
 *  For durations less than 100 µs a busy loop is used; otherwise the
 *  function yields to the scheduler. Calibrates the calling thread's timer
 *  on first use, before starting the requested wait.
 *
 *  @param nanoseconds Amount of time to sleep or busy-loop.
 */
void quicksand_sleep(double nanoseconds);

/* -------------------------------------------------------------------------- */
/* Convenience inline helpers                                                 */
/* -------------------------------------------------------------------------- */

/** @brief Return the elapsed time in nanoseconds since @p initial_timestamp.
 *
 *  @param initial_timestamp Start timestamp.
 *
 *  @return Elapsed nanoseconds.
 */
static inline double quicksand_elapsed(uint64_t initial_timestamp) {
	return quicksand_ns(quicksand_now(), initial_timestamp);
}

/** @brief Return the number of remaining messages in the buffer.
 *
 *  @param connection Connection object.
 *
 *  @return Number of unread messages.
 */
static inline uint64_t quicksand_read_remaining(quicksand_connection *connection) {
	/* Relaxed polling snapshot; no payload synchronization is implied. */
	return atomic_load_explicit(&connection->buffer->index, memory_order_relaxed)
			- connection->read_index;
}

/** @brief Read the most recent message if new messages are available.
 *
 *  If there are newer messages, this function updates @p read_index to the
 *  latest slot and reads that message.
 *
 *  @param connection   Connection object.
 *  @param message      Buffer to receive the message.
 *  @param message_size (IN/OUT) Maximum size to read; overwritten with the
 *                      number of bytes actually read.
 *
 *  @return Result of @c quicksand_read, or -1 if no new messages were
 *          available.
 */
static inline int64_t quicksand_read_latest(quicksand_connection *connection,
					    uint8_t *message, int64_t *message_size) {
	/* Use a single relaxed polling snapshot to select the latest message. */
	uint64_t index = atomic_load_explicit(&connection->buffer->index, memory_order_relaxed);
	if(connection->read_index < index) {
		connection->read_index = index - 1;
		return quicksand_read(connection, message, message_size);
	}
	return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* QUICKSAND_H */
