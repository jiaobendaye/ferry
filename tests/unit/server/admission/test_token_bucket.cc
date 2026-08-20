#include <gtest/gtest.h>
#include "admission/token_bucket.h"

namespace
{

struct FakeClock
{
	ferry::TokenBucket::TimePoint now;

	ferry::TokenBucket::TimePoint operator()() { return now; }

	void advance_ms(long long ms)
	{
		now += std::chrono::milliseconds(ms);
	}
};

TEST(TokenBucket, DisabledModeGrantsEverything)
{
	FakeClock clock;
	ferry::TokenBucket bucket(0, 30, [&] { return clock(); });

	EXPECT_TRUE(bucket.disabled());
	auto v = bucket.reserve(1LL << 40);
	EXPECT_FALSE(v.rejected);
	EXPECT_EQ(v.wait.count(), 0);
}

TEST(TokenBucket, FreshBucketHasOneSecondBurst)
{
	FakeClock clock;
	ferry::TokenBucket bucket(1000000, 30, [&] { return clock(); });	/* 1 MB/s */

	/* capacity = 1s of tokens = 1 MB */
	auto v = bucket.reserve(500000);
	EXPECT_FALSE(v.rejected);
	EXPECT_EQ(v.wait.count(), 0);
}

TEST(TokenBucket, DeficitProducesCorrectWait)
{
	FakeClock clock;
	ferry::TokenBucket bucket(1000000, 30, [&] { return clock(); });

	/* 1.5 MB against a 1 MB bucket: 0.5 MB deficit at 1 MB/s = 500 ms */
	auto v = bucket.reserve(1500000);
	EXPECT_FALSE(v.rejected);
	EXPECT_NEAR(v.wait.count(), 500, 1);
}

TEST(TokenBucket, RefillAfterWaiting)
{
	FakeClock clock;
	ferry::TokenBucket bucket(1000000, 30, [&] { return clock(); });

	bucket.reserve(1000000);		/* drain to 0 */
	clock.advance_ms(1000);			/* accrues 1 MB */

	auto v = bucket.reserve(1000000);
	EXPECT_FALSE(v.rejected);
	EXPECT_EQ(v.wait.count(), 0);
}

TEST(TokenBucket, OverMaxWaitRejectsWithoutStateChange)
{
	FakeClock clock;
	ferry::TokenBucket bucket(1000000, 1, [&] { return clock(); });	/* max_wait 1s */

	/* 5 MB deficit -> 5 s wait > 1 s max -> reject */
	auto v = bucket.reserve(5000000);
	EXPECT_TRUE(v.rejected);

	/* rejection must not consume: a small request is still immediate */
	auto v2 = bucket.reserve(100);
	EXPECT_FALSE(v2.rejected);
	EXPECT_EQ(v2.wait.count(), 0);
}

TEST(TokenBucket, WaitForApprovedRequestEqualsDeficit)
{
	FakeClock clock;
	ferry::TokenBucket bucket(2000000, 30, [&] { return clock(); });	/* 2 MB/s */

	/* capacity 2 MB; request 3 MB -> deficit 1 MB at 2 MB/s = 500 ms */
	auto v = bucket.reserve(3000000);
	EXPECT_FALSE(v.rejected);
	EXPECT_NEAR(v.wait.count(), 500, 1);
}

TEST(TokenBucket, ZeroOrNegativeChargeIsFreePass)
{
	FakeClock clock;
	ferry::TokenBucket bucket(1000, 30, [&] { return clock(); });

	EXPECT_FALSE(bucket.reserve(0).rejected);
	EXPECT_FALSE(bucket.reserve(-5).rejected);

	/* bucket untouched: full burst still available */
	auto v = bucket.reserve(1000);
	EXPECT_FALSE(v.rejected);
	EXPECT_EQ(v.wait.count(), 0);
}

TEST(TokenBucket, UnitsAreCallerDefined)
{
	/* QPS-style bucket: 5 units/s, one unit per "request" */
	FakeClock clock;
	ferry::TokenBucket bucket(5, 30, [&] { return clock(); });

	for (int i = 0; i < 5; i++)		/* 5 units of burst */
		EXPECT_EQ(bucket.reserve(1).wait.count(), 0);

	/* 6th unit in the same second: 1 unit deficit at 5/s = 200 ms */
	auto v = bucket.reserve(1);
	EXPECT_FALSE(v.rejected);
	EXPECT_NEAR(v.wait.count(), 200, 1);
}

} // namespace
