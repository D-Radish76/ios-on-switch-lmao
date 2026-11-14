// SPDX-License-Identifier: MIT
/* The deadline's stats line. Debug kernels add the learned frames' GPU time: `expected` is a peak tracker
 * (tgm_expect), so the line sets them beside it. One caller at a time (the window's winner, tgm_stats_tick). */
#include <core/device.h>
#include "TegraGM20BGpu.h"
#include "TegraGM20BSched.h"
#include "TegraGM20BCostHist.h"
#include "deadline_stats.h"

#if TGM_STATS
/* The gate's: the histogram and sums only grow; the maximum is the window's the line last asked for */
static struct {
	u32 hist[TGM_COST_BUCKETS];
	u64 n, sum, max;
	u32 max_window, window;
} cost;
#endif

void tgm_deadline_learned(uint64_t ns)
{
#if TGM_STATS
	const u32 window = READ_ONCE(cost.window);
	const u32 bucket = tgm_cost_bucket(ns);

	WRITE_ONCE(cost.hist[bucket], cost.hist[bucket] + 1);
	WRITE_ONCE(cost.n, cost.n + 1);
	WRITE_ONCE(cost.sum, cost.sum + ns);
	if (cost.max_window != window) {
		cost.max_window = window;
		WRITE_ONCE(cost.max, 0);
	}
	if (ns > cost.max)
		WRITE_ONCE(cost.max, ns);
#else
	(void)ns;
#endif
}

static struct {
	struct tgm_deadline_counts counts;
#if TGM_STATS
	u32 hist[TGM_COST_BUCKETS];
	u64 n, sum;
#endif
} seen;

void tgm_deadline_stats(char *buf, unsigned int len)
{
	struct tgm_deadline_counts now;
	struct tgm_deadline_counts *was = &seen.counts;
	struct tgm_deadline_line line;
	u64 learned = 0;
	u64 mean = 0;
	u64 p95 = 0;
	u64 max = 0;

	if (!tgm_deadline_counts(&now)) {
		scnprintf(buf, len, "deadline off");
		return;
	}
#if TGM_STATS
	{
		u32 hist[TGM_COST_BUCKETS];
		u64 sum = 0;
		unsigned int i = 0;

		for (i = 0; i < TGM_COST_BUCKETS; i++) {
			const u32 n = READ_ONCE(cost.hist[i]);

			hist[i] = n - seen.hist[i];
			seen.hist[i] = n;
		}
		learned = READ_ONCE(cost.n) - seen.n;
		seen.n += learned;
		sum = READ_ONCE(cost.sum) - seen.sum;
		seen.sum += sum;
		mean = learned ? sum / learned / 1000 : 0;
		p95 = tgm_cost_percentile(hist, learned, 950);
		max = READ_ONCE(cost.max) / 1000;
		WRITE_ONCE(cost.window, cost.window + 1);
	}
#endif
	line = (struct tgm_deadline_line){
		.gained = {.frames = now.frames - was->frames,
			   .missed_app = now.missed_app - was->missed_app,
			   .missed_clear = now.missed_clear - was->missed_clear,
			   .windows = now.windows - was->windows,
			   .empty = now.empty - was->empty,
			   .expired = now.expired - was->expired,
			   .kick_opens = now.kick_opens - was->kick_opens,
			   .frame_end_runs = now.frame_end_runs - was->frame_end_runs,
			   .timer_runs = now.timer_runs - was->timer_runs,
			   .wake_runs = now.wake_runs - was->wake_runs,
			   .expected = now.expected},
		.mean_us = mean,
		.p95_us = p95,
		.max_us = max,
		.learned = learned,
	};
	tgm_deadline_format(buf, len, &line);
	*was = now;
}
