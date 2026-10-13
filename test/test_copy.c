#define _POSIX_C_SOURCE 200809L
#include "../src/quicksand_crc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* Independent bit-at-a-time oracle; no production tables or intrinsics. */
static uint32_t reference_crc(const unsigned char *s, size_t n, uint32_t previous) {
	uint32_t crc = ~previous;
	while(n--) {
		crc ^= *s++;
		for(unsigned bit = 0; bit < 8; ++bit) {
			crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1)));
		}
	}
	return ~crc;
}

static void test_incremental(void) {
	unsigned char src[577], dst[577];
	assert(fast_memcpy_crc32c(NULL, NULL, 0, 0) == 0);
	assert(fast_memcpy_crc32c(NULL, NULL, 0, 0x12345678) == 0x12345678);
	assert(fast_memcpy_crc32c(dst, (const unsigned char *) "123456789", 9, 0) == 0xe3069283);
	for(size_t i = 0; i < sizeof(src); ++i) {
		src[i] = (unsigned char) (i * 31 + i / 7);
	}
	const uint32_t seeds[] = {0, 1, 0x12345678, UINT32_MAX};
	for(size_t j = 0; j < sizeof(seeds) / sizeof(seeds[0]); ++j) {
		uint32_t expected = reference_crc(src, sizeof(src), seeds[j]);
		for(size_t split = 0; split <= sizeof(src); ++split) {
			uint32_t crc = fast_memcpy_crc32c(dst, src, split, seeds[j]);
			crc = fast_memcpy_crc32c(dst + split, src + split, sizeof(src) - split, crc);
			assert(crc == expected);
			assert(memcmp(src, dst, sizeof(src)) == 0);
		}
	}
}

static void check_copy(size_t n, size_t so, size_t doff) {
	unsigned char *src = calloc(n + 128, 1), *dst = malloc(n + 128);
	assert(src && dst);
	for(size_t i = 0; i < n + 128; ++i) {
		src[i] = (unsigned char) (i * 71 + i / 11);
	}
	memset(dst, 0xa5, n + 128);
	assert(fast_memcpy_crc32c(dst + doff, src + so, n, 0) == reference_crc(src + so, n, 0));
	for(size_t i = 0; i < n + 128; ++i) {
		assert(dst[i] == (i >= doff && i < doff + n ? src[so + i - doff] : 0xa5));
		assert(src[i] == (unsigned char) (i * 71 + i / 11));
	}
	free(src);
	free(dst);
}

#ifndef _WIN32
static void guard_pages(void) {
	size_t page = (size_t) sysconf(_SC_PAGESIZE);
	int fd = open("/dev/zero", O_RDWR);
	assert(fd >= 0);
	unsigned char *src = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE, fd, 0);
	unsigned char *dst = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE, fd, 0);
	assert(close(fd) == 0);
	assert(src != MAP_FAILED && dst != MAP_FAILED);
	assert(mprotect(src + page, page, PROT_READ | PROT_WRITE) == 0);
	assert(mprotect(dst + page, page, PROT_READ | PROT_WRITE) == 0);
	memset(src + page, 0x57, page);
	for(size_t n = 0; n <= page; ++n) {
		uint32_t expected = reference_crc(src + page, n, 0);
		/* Ends adjacent to guard pages, including zero length on a guard. */
		assert(fast_memcpy_crc32c(dst + 2 * page - n, src + 2 * page - n, n, 0) == expected);
		assert(memcmp(dst + 2 * page - n, src + 2 * page - n, n) == 0);
		/* Independent start/end alignment and protection against underruns. */
		assert(fast_memcpy_crc32c(dst + page, src + 2 * page - n, n, 0) == expected);
		assert(memcmp(dst + page, src + 2 * page - n, n) == 0);
		assert(fast_memcpy_crc32c(dst + 2 * page - n, src + page, n, 0) == expected);
		assert(memcmp(dst + 2 * page - n, src + page, n) == 0);
	}
	assert(munmap(src, page * 3) == 0);
	assert(munmap(dst, page * 3) == 0);
}
#endif

int main(void) {
	test_incremental();
	/* Every source/destination cache-line offset and all small boundaries. */
	for(size_t n = 0; n <= 320; ++n) {
		for(size_t so = 0; so < 64; ++so) {
			for(size_t d = 0; d < 64; ++d) {
				check_copy(n, so, d);
			}
		}
	}
	const size_t sizes[] = {511, 512, 513, 1023, 1024, 1025, 4095, 4096, 4097, 32767, 32768, 32769, 1048576};
	for(size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
		for(size_t offset = 0; offset < 64; ++offset) {
			check_copy(sizes[i], offset, 63 - offset);
		}
	}
#ifndef _WIN32
	guard_pages();
#endif
	puts("Copy/CRC alignment, incremental, canary and guard-page tests passed (" QUICKSAND_CRC_BACKEND ")");
}
