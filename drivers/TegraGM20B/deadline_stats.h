/* SPDX-License-Identifier: MIT */
/* The deadline's and the hold's stats lines (deadline_stats.c, hold_stats.c) and what they read. The counts only grow,
 * written in the GPU interrupt work loop's gate; a line prints what they gained since the line before. */
#pragma once
#include <stdint.h>

struct tgm_deadline_counts {
	uint32_t frames, missed_app, missed_clear, windows, empty, expired;
	/* debug kernels: windows an app's kick opened; the gate's runs at a frame end, from the owner's timer, from a wake */
	uint32_t kick_opens, frame_end_runs, timer_runs, wake_runs;
	uint64_t expected;
};

/* 0 before the deadline is set up */
int tgm_deadline_counts(struct tgm_deadline_counts *c);
/* The gate, debug kernels: a frame's learned GPU time, for the line's mean, p95 and maximum */
void tgm_deadline_learned(uint64_t ns);

/* A compositor frame whose present completed past its vblank, as its present found it */
struct tgm_deadline_miss {
	uint64_t late_ns;      /* the present's completion past the vblank */
	uint64_t kick_lead_ns; /* the compositor's last kick before the vblank (0: at or after it) */
	uint64_t gpu_ns;       /* the frame's GPU time (0: not all of it is this frame's) */
	uint64_t expected_ns;  /* the GPU time a frame learned */
	int app_kicked;        /* an app kicked since the last present */
	int app_held;          /* an app's job held the GPU at one of the compositor's kicks */
};

/* Why it missed, first cause first: an app on the GPU at a compositor kick, the compositor's own GPU time past what
 * it learned, a last kick too late for its GPU time, an app's work between its kicks, or none of them (the present
 * itself late) */
static inline const char *tgm_deadline_miss_cause(const struct tgm_deadline_miss *m)
{
	const uint64_t gpu = m->gpu_ns ? m->gpu_ns : m->expected_ns;

	if (m->app_held)
		return "an app's job held the GPU at a compositor kick";
	if (m->gpu_ns && m->expected_ns && m->gpu_ns > m->expected_ns + m->expected_ns / 4)
		return "the compositor's own GPU time past what it learned";
	if (m->kick_lead_ns < gpu)
		return "the compositor's last kick too late for its GPU time";
	if (m->app_kicked)
		return "an app's work on the GPU between the compositor's kicks";
	return "the present completed late with GPU time to spare";
}

/* The gate, debug kernels: one line for a missed frame naming its cause (the first 32, then every 64th) */
void tgm_deadline_missed(const struct tgm_deadline_miss *m);

struct tgm_hold_counts {
	uint32_t holds, preempts, busy, turned, hopeless, samples, checks;
};

void tgm_hold_counts(struct tgm_hold_counts *c);
uint32_t tgm_hold_turns(unsigned int chid);
/* Debug kernels: the channel's process (its first submission's), and in the gate a preemption latency sampled for it */
void tgm_hold_owner(unsigned int chid);
void tgm_hold_landed(unsigned int chid, uint64_t asked_pt, uint64_t start_pt, uint64_t sample);
