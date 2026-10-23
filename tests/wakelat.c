/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wakelat: measure how late a periodic sleeper wakes up.
 *
 * Sleeps until an absolute deadline every PERIOD_US microseconds, N times, and
 * records how far past each deadline it actually ran. It switches itself to
 * SCHED_EXT first, so it must be started inside the cgroup it should be
 * measured in (the test script does that).
 *
 *   wakelat [N] [PERIOD_US]       defaults: 2000 1000
 *
 * Prints one line:  n=.. avg_us=.. p50_us=.. p99_us=.. max_us=..
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>

#ifndef SCHED_EXT
#define SCHED_EXT 7
#endif

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static uint64_t ts_ns(const struct timespec *t)
{
	return (uint64_t)t->tv_sec * 1000000000ULL + t->tv_nsec;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 2000;
	long period_us = argc > 2 ? atol(argv[2]) : 1000;
	struct sched_param sp = { .sched_priority = 0 };
	struct timespec next, now;
	uint64_t *lat, sum = 0;
	int i;

	if (n <= 0 || period_us <= 0) {
		fprintf(stderr, "usage: %s [N] [PERIOD_US]\n", argv[0]);
		return 1;
	}
	if (sched_setscheduler(0, SCHED_EXT, &sp)) {
		perror("sched_setscheduler(SCHED_EXT)");
		return 1;
	}

	lat = malloc(sizeof(*lat) * n);
	if (!lat)
		return 1;

	clock_gettime(CLOCK_MONOTONIC, &next);
	for (i = 0; i < n; i++) {
		next.tv_nsec += period_us * 1000L;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		clock_gettime(CLOCK_MONOTONIC, &now);
		lat[i] = ts_ns(&now) > ts_ns(&next) ? ts_ns(&now) - ts_ns(&next) : 0;
		sum += lat[i];
	}

	qsort(lat, n, sizeof(*lat), cmp_u64);
	printf("n=%d avg_us=%llu p50_us=%llu p99_us=%llu max_us=%llu\n", n,
	       (unsigned long long)(sum / n / 1000),
	       (unsigned long long)(lat[n / 2] / 1000),
	       (unsigned long long)(lat[(n * 99) / 100] / 1000),
	       (unsigned long long)(lat[n - 1] / 1000));
	return 0;
}
