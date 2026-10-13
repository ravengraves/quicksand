#define _POSIX_C_SOURCE 200809L
#include "../src/quicksand_crc.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile uint32_t checksum_sink;
typedef uint32_t (*copy_fn)(unsigned char *, const unsigned char *, size_t);

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

__attribute__((noinline)) static uint32_t production_copy(unsigned char *d, const unsigned char *s, size_t n) {
	uint32_t crc = fast_memcpy_crc32c(d, s, n, 0);
	__asm__ volatile("" : : "r"(d) : "memory");
	return crc;
}

__attribute__((noinline)) static uint32_t library_copy(unsigned char *d, const unsigned char *s, size_t n) {
	memcpy(d, s, n);
	__asm__ volatile("" : : "r"(d) : "memory");
	return 0;
}

static double measure(copy_fn copy, unsigned char *d, const unsigned char *s,
		      size_t n, size_t stride, size_t slots, size_t iterations) {
	uint32_t checksum = 0;
	size_t slot = 0;
	double start = now();
	for(size_t i = 0; i < iterations; ++i) {
		checksum += copy(d + slot * stride, s + slot * stride, n);
		if(++slot == slots) {
			slot = 0;
		}
	}
	double elapsed = now() - start;
	checksum_sink += checksum;
	return elapsed;
}

static double median(double values[5]) {
	for(size_t i = 1; i < 5; ++i) {
		for(size_t j = i; j > 0 && values[j] < values[j - 1]; --j) {
			double tmp = values[j];
			values[j] = values[j - 1];
			values[j - 1] = tmp;
		}
	}
	return values[2];
}

int main(void) {
	const size_t sizes[] = {8, 512, 1024, 16384, 1048576};
	const char *labels[] = {"8 B", "512 B", "1 KiB", "16 KiB", "1 MiB"};
	const size_t offsets[][2] = {{0, 0}, {0, 16}, {16, 0}, {1, 3}};
	const char *alignments[] = {"aligned", "ring write +16", "ring read +16", "unaligned 1/3"};
	const copy_fn methods[] = {library_copy, production_copy};
	const size_t arena = 64 * 1024 * 1024;
	unsigned char *s = NULL, *d = NULL;
	if(posix_memalign((void **) &s, 64, arena) || posix_memalign((void **) &d, 64, arena)) {
		free(s);
		free(d);
		return 1;
	}
	memset(s, 0x51, arena);
	memset(d, 0, arena);
	printf("CRC backend: %s; median of five repetitions; alternating method order\n", QUICKSAND_CRC_BACKEND);
	puts("Resident: one buffer pair (up to 2 MiB). Streaming: two 64 MiB arenas.");
	puts("size    working set alignment       libc ns/copy CRC ns/copy libc GiB/s CRC GiB/s overhead ns      %");
	for(size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
		for(int streaming = 0; streaming < 2; ++streaming) {
			/* Leave room for all offsets while keeping every slot 64-byte aligned. */
			size_t n = sizes[k], stride = (n + 16 + 63) & ~(size_t) 63;
			size_t slots = streaming ? arena / stride : 1;
			for(size_t o = 0; o < sizeof(offsets) / sizeof(offsets[0]); ++o) {
				size_t iterations[2] = {slots, slots};
				double samples[2][5];
				/* Warm up/calibrate to >=8 ms, visiting every streaming slot. */
				for(int m = 0; m < 2; ++m) {
					while(measure(methods[m], d + offsets[o][1], s + offsets[o][0],
						      n, stride, slots, iterations[m])
					      < 0.008) {
						iterations[m] *= 2;
					}
				}
				for(int rep = 0; rep < 5; ++rep) {
					for(int turn = 0; turn < 2; ++turn) {
						int m = (rep + turn + o + k) & 1;
						samples[m][rep] = 1e9 * measure(methods[m], d + offsets[o][1], s + offsets[o][0], n, stride, slots, iterations[m]) / iterations[m];
					}
				}
				double libc_ns = median(samples[0]), crc_ns = median(samples[1]);
				printf("%-7s %-11s %-15s %12.2f %11.2f %10.2f %9.2f %+11.2f %+6.1f\n",
				       labels[k], streaming ? "streaming" : "resident", alignments[o], libc_ns, crc_ns,
				       n / libc_ns * (1e9 / 1073741824.0), n / crc_ns * (1e9 / 1073741824.0),
				       crc_ns - libc_ns, 100 * (crc_ns / libc_ns - 1));
			}
		}
	}
	printf("Checksum sink: %08x\n", (unsigned) checksum_sink);
	free(s);
	free(d);
	return 0;
}
