/*
 * L1 thread hammers: real threads, frozen fake clocks. These prove the
 * two concurrency-critical invariants deterministically:
 *
 *  - no over-grant: with time frozen, a bucket can admit at most
 *    capacity + rate x max_wait tokens in total (beyond that, the
 *    computed wait exceeds max_wait and reserve() must reject);
 *  - no leak: balanced acquire/release leaves gauges at zero and keyed
 *    maps empty.
 *
 * Run under TSan (xmake f --tsan=y) to also catch the data races these
 * invariants alone cannot see.
 */
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

#include "concurrency_limiter.h"
#include "token_bucket.h"

namespace
{

TEST(Hammer, TokenBucketNeverOvergrantsAtFrozenTime)
{
	/* frozen clock: no refill ever accrues, so the total charge admitted
	   across all threads has an exact ceiling:
	   capacity(1000) + rate(1000) x max_wait(10 s) = 11000 tokens */
	const long long rate = 1000;
	const long long max_wait_sec = 10;
	const long long ceiling = rate + rate * max_wait_sec;

	ferry::TokenBucket::TimePoint frozen;
	ferry::TokenBucket bucket(rate, max_wait_sec, [&] { return frozen; });

	const int threads = 4;
	const int iterations = 50000;
	std::atomic<long long> admitted{0};
	std::atomic<long long> rejected{0};

	auto work = [&]() {
		for (int i = 0; i < iterations; i++)
		{
			auto v = bucket.reserve(1);
			if (v.rejected)
				rejected.fetch_add(1, std::memory_order_relaxed);
			else
				admitted.fetch_add(1, std::memory_order_relaxed);
		}
	};

	std::vector<std::thread> ts;
	for (int i = 0; i < threads; i++)
		ts.emplace_back(work);
	for (auto& t : ts)
		t.join();

	EXPECT_LE(admitted.load(), ceiling);	/* the exact no-over-grant bound */
	EXPECT_GE(admitted.load(), rate);		/* the burst alone is granted */
	EXPECT_EQ(admitted.load() + rejected.load(),
			  (long long)threads * iterations);
	EXPECT_GT(rejected.load(), 0);			/* the ceiling was hit */
}

TEST(Hammer, ConcurrencyGlobalNeverExceedsCapAndNeverLeaks)
{
	const int cap = 7;
	ferry::ConcurrencyLimiter lim(cap);

	const int threads = 4;
	const int iterations = 30000;
	std::atomic<int> in_section{0};
	std::atomic<int> peak{0};
	std::atomic<long long> grants{0};

	auto work = [&]() {
		for (int i = 0; i < iterations; i++)
		{
			if (!lim.try_acquire())
				continue;

			int v = in_section.fetch_add(1, std::memory_order_acq_rel) + 1;
			int p = peak.load(std::memory_order_relaxed);
			while (v > p &&
				   !peak.compare_exchange_weak(p, v,
											   std::memory_order_relaxed))
			{
			}

			in_section.fetch_sub(1, std::memory_order_acq_rel);
			lim.release();
			grants.fetch_add(1, std::memory_order_relaxed);
		}
	};

	std::vector<std::thread> ts;
	for (int i = 0; i < threads; i++)
		ts.emplace_back(work);
	for (auto& t : ts)
		t.join();

	EXPECT_LE(peak.load(), cap);			/* no over-grant, ever */
	EXPECT_GT(grants.load(), 0);
	EXPECT_EQ(lim.current(), 0);			/* no leak */
}

TEST(Hammer, ConcurrencyKeyedNeverExceedsCapAndDrainsEmpty)
{
	const int cap = 3;
	const int keys = 5;
	ferry::ConcurrencyLimiter lim(cap);

	const int threads = 4;
	const int iterations = 20000;
	std::vector<std::atomic<int>> in_section(keys);
	std::atomic<int> violation{0};

	auto work = [&]() {
		for (int i = 0; i < iterations; i++)
		{
			std::string key = "ip" + std::to_string(i % keys);
			if (!lim.try_acquire(key))
				continue;

			int v = in_section[i % keys].fetch_add(1) + 1;
			if (v > cap)
				violation.fetch_add(1);

			in_section[i % keys].fetch_sub(1);
			lim.release(key);
		}
	};

	std::vector<std::thread> ts;
	for (int i = 0; i < threads; i++)
		ts.emplace_back(work);
	for (auto& t : ts)
		t.join();

	EXPECT_EQ(violation.load(), 0);			/* per-key cap held */
	EXPECT_EQ(lim.size(), 0u);				/* erase-on-zero drained all */
	for (int k = 0; k < keys; k++)
		EXPECT_EQ(lim.current("ip" + std::to_string(k)), 0);
}

} // namespace
