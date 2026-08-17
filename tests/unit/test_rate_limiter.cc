#include <gtest/gtest.h>
#include "rate_limiter.h"

namespace
{

struct FakeClock
{
	ferry::RateLimiter::TimePoint now;

	ferry::RateLimiter::TimePoint operator()() { return now; }

	void advance_ms(long long ms)
	{
		now += std::chrono::milliseconds(ms);
	}
};

TEST(RateLimiter, DisabledModeGrantsEverything)
{
	FakeClock clock;
	ferry::RateLimiter limiter(0, 30, [&] { return clock(); });

	EXPECT_TRUE(limiter.disabled());
	auto v = limiter.reserve("ip1", 1LL << 40);
	EXPECT_FALSE(v.rejected);
	EXPECT_EQ(v.wait.count(), 0);
}

TEST(RateLimiter, FreshBucketHasOneSecondBurst)
{
	FakeClock clock;
	ferry::RateLimiter limiter(1000000, 30, [&] { return clock(); });	/* 1 MB/s */

	/* capacity = 1s of tokens = 1 MB */
	auto v = limiter.reserve("ip1", 500000);
	EXPECT_FALSE(v.rejected);
	EXPECT_EQ(v.wait.count(), 0);
}

TEST(RateLimiter, DeficitProducesCorrectWait)
{
	FakeClock clock;
	ferry::RateLimiter limiter(1000000, 30, [&] { return clock(); });

	/* 1.5 MB against a 1 MB bucket: 0.5 MB deficit at 1 MB/s = 500 ms */
	auto v = limiter.reserve("ip1", 1500000);
	EXPECT_FALSE(v.rejected);
	EXPECT_NEAR(v.wait.count(), 500, 1);
}

TEST(RateLimiter, RefillAfterWaiting)
{
	FakeClock clock;
	ferry::RateLimiter limiter(1000000, 30, [&] { return clock(); });

	limiter.reserve("ip1", 1000000);	/* drain to 0 */
	clock.advance_ms(1000);				/* accrues 1 MB */

	auto v = limiter.reserve("ip1", 1000000);
	EXPECT_FALSE(v.rejected);
	EXPECT_EQ(v.wait.count(), 0);
}

TEST(RateLimiter, OverMaxWaitRejectsWithoutStateChange)
{
	FakeClock clock;
	ferry::RateLimiter limiter(1000000, 1, [&] { return clock(); });	/* max_wait 1s */

	/* 5 MB deficit -> 5 s wait > 1 s max -> reject */
	auto v = limiter.reserve("ip1", 5000000);
	EXPECT_TRUE(v.rejected);

	/* rejection must not consume: a small request is still immediate */
	auto v2 = limiter.reserve("ip1", 100);
	EXPECT_FALSE(v2.rejected);
	EXPECT_EQ(v2.wait.count(), 0);
}

TEST(RateLimiter, WaitForApprovedRequestEqualsDeficit)
{
	FakeClock clock;
	ferry::RateLimiter limiter(2000000, 30, [&] { return clock(); });	/* 2 MB/s */

	/* capacity 2 MB; request 3 MB -> deficit 1 MB at 2 MB/s = 500 ms */
	auto v = limiter.reserve("ip1", 3000000);
	EXPECT_FALSE(v.rejected);
	EXPECT_NEAR(v.wait.count(), 500, 1);
}

TEST(RateLimiter, IPsAreIndependent)
{
	FakeClock clock;
	ferry::RateLimiter limiter(1000000, 30, [&] { return clock(); });

	auto a = limiter.reserve("ipA", 1500000);
	auto b = limiter.reserve("ipB", 500000);
	EXPECT_GT(a.wait.count(), 0);
	EXPECT_EQ(b.wait.count(), 0);
}

TEST(RateLimiter, IdleSweepReclaimsEntries)
{
	FakeClock clock;
	ferry::RateLimiter limiter(1000000, 30, [&] { return clock(); });

	limiter.reserve("ip1", 100);
	limiter.reserve("ip2", 100);
	EXPECT_EQ(limiter.size(), 2u);

	clock.advance_ms(300000);				/* 5 minutes idle */
	limiter.sweep(std::chrono::minutes(4));
	EXPECT_EQ(limiter.size(), 0u);
}

TEST(RateLimiter, ActiveEntrySurvivesSweep)
{
	FakeClock clock;
	ferry::RateLimiter limiter(1000000, 30, [&] { return clock(); });

	limiter.reserve("ip1", 100);
	clock.advance_ms(60000);
	limiter.reserve("ip1", 100);			/* touch again at +60 s */
	clock.advance_ms(60000);				/* idle only 60 s now */

	limiter.sweep(std::chrono::minutes(5));
	EXPECT_EQ(limiter.size(), 1u);
}

} // namespace
