#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include "concurrency_limiter.h"

namespace
{

TEST(ConcurrencyLimiter, DisabledGrantsEverything)
{
	ferry::ConcurrencyLimiter lim(0);
	EXPECT_TRUE(lim.disabled());
	for (int i = 0; i < 100; i++)
		EXPECT_TRUE(lim.try_acquire());
	EXPECT_EQ(lim.current(), 0);		/* disabled: no counting */
}

TEST(ConcurrencyLimiter, GlobalCapEnforced)
{
	ferry::ConcurrencyLimiter lim(3);

	EXPECT_TRUE(lim.try_acquire());
	EXPECT_TRUE(lim.try_acquire());
	EXPECT_TRUE(lim.try_acquire());
	EXPECT_FALSE(lim.try_acquire());	/* cap reached */
	EXPECT_EQ(lim.current(), 3);

	lim.release();
	EXPECT_TRUE(lim.try_acquire());		/* slot freed */
	EXPECT_EQ(lim.current(), 3);
}

TEST(ConcurrencyLimiter, BalancedAcquireReleaseLeavesZero)
{
	ferry::ConcurrencyLimiter lim(10);

	for (int i = 0; i < 1000; i++)
	{
		ASSERT_TRUE(lim.try_acquire());
		lim.release();
	}
	EXPECT_EQ(lim.current(), 0);
}

TEST(ConcurrencyLimiter, KeyedCapsArePerKey)
{
	ferry::ConcurrencyLimiter lim(2);

	EXPECT_TRUE(lim.try_acquire("ipA"));
	EXPECT_TRUE(lim.try_acquire("ipA"));
	EXPECT_FALSE(lim.try_acquire("ipA"));	/* ipA at cap */

	EXPECT_TRUE(lim.try_acquire("ipB"));	/* ipB unaffected */
	EXPECT_EQ(lim.current("ipA"), 2);
	EXPECT_EQ(lim.current("ipB"), 1);
	EXPECT_EQ(lim.size(), 2u);
}

TEST(ConcurrencyLimiter, KeyedEntryErasedAtZero)
{
	ferry::ConcurrencyLimiter lim(2);

	lim.try_acquire("ipA");
	lim.try_acquire("ipA");
	EXPECT_EQ(lim.size(), 1u);

	lim.release("ipA");
	EXPECT_EQ(lim.size(), 1u);			/* still one in flight */
	EXPECT_EQ(lim.current("ipA"), 1);

	lim.release("ipA");
	EXPECT_EQ(lim.size(), 0u);			/* erased at zero */
	EXPECT_EQ(lim.current("ipA"), 0);

	/* re-acquire recreates the entry cleanly */
	EXPECT_TRUE(lim.try_acquire("ipA"));
	EXPECT_EQ(lim.current("ipA"), 1);
}

TEST(ConcurrencyLimiter, DisabledKeyedModeTracksNothing)
{
	ferry::ConcurrencyLimiter lim(0);	/* disabled keyed use */
	EXPECT_TRUE(lim.try_acquire("ipA"));
	EXPECT_EQ(lim.size(), 0u);			/* disabled: nothing tracked */
	lim.release("ipA");					/* harmless */
	EXPECT_EQ(lim.size(), 0u);
}

TEST(ConcurrencyLimiter, UnbalancedReleaseIsIgnored)
{
	ferry::ConcurrencyLimiter lim(2);
	lim.release("never-acquired");		/* must not corrupt state */
	EXPECT_EQ(lim.size(), 0u);
	EXPECT_TRUE(lim.try_acquire("never-acquired"));
}

TEST(ConcurrencyLimiter, ConcurrentGlobalNeverExceedsCap)
{
	ferry::ConcurrencyLimiter lim(7);
	std::atomic<int> in{0};
	std::atomic<int> peak{0};

	auto work = [&]() {
		for (int i = 0; i < 2000; i++)
		{
			if (lim.try_acquire())
			{
				int v = in.fetch_add(1) + 1;
				int p = peak.load();
				while (v > p && !peak.compare_exchange_weak(p, v)) {}
				in.fetch_sub(1);
				lim.release();
			}
		}
	};

	std::vector<std::thread> ts;
	for (int i = 0; i < 4; i++)
		ts.emplace_back(work);
	for (auto& t : ts)
		t.join();

	EXPECT_LE(peak.load(), 7);
	EXPECT_EQ(lim.current(), 0);
}

} // namespace
