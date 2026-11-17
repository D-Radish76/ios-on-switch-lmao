// SPDX-License-Identifier: MIT
// A producer that failed or whose client died ends the waits on it as its completion does: the waiting job is admitted
// and runs on what the producer left. The compositor's job waiting on a killed app's event or write was refused
// (EventPrepare, EventAppend, EventInherit), which panics the device, or failed after its admission, which ends
// backboardd.
#include "switch_gpu_event_fixture.h"

namespace
{
constexpr int kGone = -19, kKilled = -5;

void setStatus(const switch_gpu_backing_token &t, int status)
{
	std::lock_guard held(tokensLock);
	tokens[key(t)].execution = tokens[key(t)].external = status;
}
}  // namespace

TEST_F(Events, AWaitOnAProducerWhoseClientDiedIsAdmitted)
{
	uint64_t value = 5;
	for (const int death : {kGone, kKilled}) {
		auto t = token();
		signal(value, t);
		setStatus(t, death);
		switch_gpu_event_use *use = nullptr;
		switch_gpu_backing_token waited{};
		EXPECT_EQ(switch_gpu_event_resolve_use(event, value, &waited, &use), 1)
			<< death << ": before the worker retires the signal";
		EXPECT_EQ(use, nullptr);
		switch_gpu_events_completed();
		pump();
		EXPECT_EQ(switch_gpu_event_resolve_use(event, value, &waited, &use), 1)
			<< death << ": after";
		EXPECT_EQ(use, nullptr);
		++value;
	}
	EXPECT_EQ(native->value, 0u);
}

TEST_F(Events, TheCompositorsJobOnAKilledClientsWriteHasNothingLeftToWaitFor)
{
	auto a = token(), b = token();
	signal(1, a);
	auto *use = resolve(1);
	auto *write = batch(use, b);
	switch_gpu_event_use_release(&use);
	commit(write);
	semantics[key(b)] = write;
	finish(b, kKilled);
	EXPECT_EQ(switch_gpu_event_waits_covers(nullptr, &b), 1);
	switch_gpu_event_waits *inherited = nullptr;
	EXPECT_EQ(switch_gpu_event_waits_prepare_dependencies(&b, 1, &inherited), 0);
	EXPECT_EQ(inherited, nullptr);
	semantics.clear();
	finish(a, kKilled);
	switch_gpu_event_waits_release(&write);
	pump();
}

TEST_F(Events, AnAcceptedWaitersOutcomeSurvivesItsProducersClientDying)
{
	auto a = token(), c = token();
	signal(1, a);
	auto *use = resolve(1);
	auto *wait = batch(use, c);
	switch_gpu_event_use_release(&use);
	int outcome = 99;
	ASSERT_EQ(switch_gpu_event_waits_completion(
			  wait,
			  [](void *context, int status) {
				  *static_cast<int *>(context) = status;
			  },
			  &outcome),
		  0);
	commit(wait);
	finish(c);
	EXPECT_EQ(outcome, 99);
	finish(a, kGone);
	EXPECT_EQ(outcome, 0) << "below 0, switch_gpu_timeline_semantic_finish fails the waiter";
	switch_gpu_event_waits_release(&wait);
	pump();
}
