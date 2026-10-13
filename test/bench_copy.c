#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
/* Include the implementation to exercise the actual static payload helper. */
#ifdef _WIN32
#include "../src/quicksand+windows.c"
#else
#include "../src/quicksand.c"
#endif

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}
__attribute__((noinline)) static void manual_copy(unsigned char *d, const unsigned char *s, size_t n) {
	fast_memcpy(d, s, n);
	__asm__ volatile("" : : "r"(d) : "memory");
}
__attribute__((noinline)) static void library_copy(unsigned char *d, const unsigned char *s, size_t n) {
	memcpy(d, s, n);
	__asm__ volatile("" : : "r"(d) : "memory");
}
int main(void) {
	const size_t sizes[] = {8, 32, 64, 256, 1024, 4096, 32768, 1048576};
	const size_t offsets[][2] = {{0, 0}, {1, 3}, {16, 0}, {63, 1}};
	puts("bytes,src_offset,dst_offset,libc_ns,manual_ns");
	for(size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
		size_t n = sizes[k], iterations = 33554432 / n;
		if(iterations < 256) {
			iterations = 256;
		}
		unsigned char *s = NULL, *d = NULL;
		if(posix_memalign((void **) &s, 64, n + 64) || posix_memalign((void **) &d, 64, n + 64)) {
			return 1;
		}
		memset(s, 0x51, n + 64);
		memset(d, 0, n + 64);
		for(size_t o = 0; o < sizeof(offsets) / sizeof(offsets[0]); ++o) {
			printf("%zu,%zu,%zu", n, offsets[o][0], offsets[o][1]);
			for(int mode = 0; mode < 2; ++mode) {
				void (*copy)(unsigned char *, const unsigned char *, size_t) = mode ? manual_copy : library_copy;
				double best = 1e30;
				for(int rep = 0; rep < 3; ++rep) {
					double start = now();
					for(size_t i = 0; i < iterations; ++i) {
						copy(d + offsets[o][1], s + offsets[o][0], n);
					}
					double ns = (now() - start) * 1e9 / iterations;
					if(ns < best) {
						best = ns;
					}
				}
				printf(",%.2f", best);
			}
			puts("");
		}
		free(s);
		free(d);
	}
}
