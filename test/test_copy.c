#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
/* Include the implementation to exercise the actual static payload helper. */
#ifdef _WIN32
#include "../src/quicksand+windows.c"
#else
#include "../src/quicksand.c"
#endif
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

static void check_copy(size_t n, size_t so, size_t doff) {
	unsigned char *src = malloc(n + 128), *dst = malloc(n + 128);
	assert(src && dst);
	for(size_t i = 0; i < n + 128; ++i) {
		src[i] = (unsigned char) (i * 71 + i / 11);
	}
	memset(dst, 0xa5, n + 128);
	fast_memcpy(dst + doff, src + so, n);
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
		/* Ends adjacent to guard pages, including zero length on a guard. */
		fast_memcpy(dst + 2 * page - n, src + 2 * page - n, n);
		assert(memcmp(dst + 2 * page - n, src + 2 * page - n, n) == 0);
		/* Independent start/end alignment and protection against underruns. */
		fast_memcpy(dst + page, src + 2 * page - n, n);
		assert(memcmp(dst + page, src + 2 * page - n, n) == 0);
		fast_memcpy(dst + 2 * page - n, src + page, n);
		assert(memcmp(dst + 2 * page - n, src + page, n) == 0);
	}
	assert(munmap(src, page * 3) == 0);
	assert(munmap(dst, page * 3) == 0);
}
#endif

int main(void) {
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
	puts("Copy alignment, length, canary and guard-page tests passed");
}
