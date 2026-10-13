#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "quicksand.h"

int main(void) {
	double mean = 0.0;
	double mean_unix = 0.0;

	struct timespec start_ts, stop_ts;
	uint64_t start, stop;

	// Sleep must initialize the timer without an earlier calibration call.
	clock_gettime(CLOCK_MONOTONIC, &start_ts);
	start = quicksand_now();
	quicksand_sleep(10e3);
	stop = quicksand_now();
	clock_gettime(CLOCK_MONOTONIC, &stop_ts);
	assert((double) (stop_ts.tv_sec - start_ts.tv_sec) * 1e9
			+ (double) (stop_ts.tv_nsec - start_ts.tv_nsec) >= 10e3);
	quicksand_ns(stop, start);
	assert(quicksand_ns(stop, start) - fabs(quicksand_ns(start, stop)) < 1e-12);
	assert(quicksand_ns(stop, start) > 0);
	assert(quicksand_ns(start, stop) < 0);

	for(int i = 0; i < 5; i += 1) {
		clock_gettime(CLOCK_MONOTONIC, &start_ts);
		start = quicksand_now();
		quicksand_sleep(10e3);
		stop = quicksand_now();
		clock_gettime(CLOCK_MONOTONIC, &stop_ts);
		mean += quicksand_ns(stop, start);
		mean_unix += (double) (stop_ts.tv_sec - start_ts.tv_sec) * 1e9
				+ (double) (stop_ts.tv_nsec - start_ts.tv_nsec);
	}
	mean *= (1.0 / 5.0);
	mean_unix *= (1.0 / 5.0);

	// printf("quicksand %f ns, unix %f ns\n", mean, mean_unix);

	assert(fabs(mean - mean_unix) < 1000.0);
	assert(mean_unix < 15e3);
}
