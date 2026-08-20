#include <gtest/gtest.h>
#include "download/backoff.h"

namespace
{

using ferry::ChunkOutcome;

ChunkOutcome classify(int status)
{
	return ferry::classify_outcome(status);
}

TEST(ClassifyOutcome, SuccessAndCompletion)
{
	EXPECT_EQ(classify(206), ChunkOutcome::SUCCESS);
	EXPECT_EQ(classify(416), ChunkOutcome::COMPLETE_416);
}

TEST(ClassifyOutcome, RateLimitedAndFatal)
{
	EXPECT_EQ(classify(429), ChunkOutcome::RATE_LIMITED);
	EXPECT_EQ(classify(403), ChunkOutcome::FATAL);
	EXPECT_EQ(classify(404), ChunkOutcome::FATAL);
}

TEST(ClassifyOutcome, NetworkFailureAndServerErrorsAreTransient)
{
	EXPECT_EQ(classify(0), ChunkOutcome::TRANSIENT);	/* no HTTP response */
	EXPECT_EQ(classify(500), ChunkOutcome::TRANSIENT);
	EXPECT_EQ(classify(502), ChunkOutcome::TRANSIENT);
	EXPECT_EQ(classify(503), ChunkOutcome::TRANSIENT);
	EXPECT_EQ(classify(504), ChunkOutcome::TRANSIENT);
	EXPECT_EQ(classify(599), ChunkOutcome::TRANSIENT);	/* 5xx boundary */
}

TEST(ClassifyOutcome, AnythingElseIsMismatch)
{
	EXPECT_EQ(classify(200), ChunkOutcome::MISMATCH);	/* Range ignored */
	EXPECT_EQ(classify(301), ChunkOutcome::MISMATCH);
	EXPECT_EQ(classify(400), ChunkOutcome::MISMATCH);
	EXPECT_EQ(classify(401), ChunkOutcome::MISMATCH);
	EXPECT_EQ(classify(405), ChunkOutcome::MISMATCH);
	EXPECT_EQ(classify(413), ChunkOutcome::MISMATCH);
	EXPECT_EQ(classify(451), ChunkOutcome::MISMATCH);
	EXPECT_EQ(classify(499), ChunkOutcome::MISMATCH);	/* 4xx boundary */
}

TEST(BackoffMs, SequenceDoublesAndCapsAt30s)
{
	EXPECT_EQ(ferry::backoff_ms(0), 500);
	EXPECT_EQ(ferry::backoff_ms(1), 1000);
	EXPECT_EQ(ferry::backoff_ms(2), 2000);
	EXPECT_EQ(ferry::backoff_ms(3), 4000);
	EXPECT_EQ(ferry::backoff_ms(4), 8000);
	EXPECT_EQ(ferry::backoff_ms(5), 16000);
	EXPECT_EQ(ferry::backoff_ms(6), 30000);		/* 32000 capped */
}

TEST(BackoffMs, StaysAtCapBeyondAttemptSix)
{
	EXPECT_EQ(ferry::backoff_ms(7), 30000);
	EXPECT_EQ(ferry::backoff_ms(8), 30000);
	EXPECT_EQ(ferry::backoff_ms(100), 30000);
}

TEST(AttemptsExhausted, BoundaryAtEight)
{
	EXPECT_FALSE(ferry::attempts_exhausted(0));
	EXPECT_FALSE(ferry::attempts_exhausted(1));
	EXPECT_FALSE(ferry::attempts_exhausted(7));
	EXPECT_TRUE(ferry::attempts_exhausted(8));
	EXPECT_TRUE(ferry::attempts_exhausted(9));
}

TEST(RateLimitedWait, RetryAfterInSecondsWinsWhenLarger)
{
	EXPECT_EQ(ferry::rate_limited_wait_ms(1, "5"), 5000);	/* > backoff 1000 */
	EXPECT_EQ(ferry::rate_limited_wait_ms(0, "5"), 5000);	/* > backoff 500 */
}

TEST(RateLimitedWait, BackoffWinsWhenLarger)
{
	EXPECT_EQ(ferry::rate_limited_wait_ms(0, "0"), 500);	/* zero seconds */
	EXPECT_EQ(ferry::rate_limited_wait_ms(3, "1"), 4000);	/* 1s < backoff 4s */
	EXPECT_EQ(ferry::rate_limited_wait_ms(6, "10"), 30000);	/* 10s < cap */
	EXPECT_EQ(ferry::rate_limited_wait_ms(6, "60"), 60000);	/* 60s > cap */
}

TEST(RateLimitedWait, UnparseableFallsBackToBackoff)
{
	EXPECT_EQ(ferry::rate_limited_wait_ms(0, ""), 500);			/* absent */
	EXPECT_EQ(ferry::rate_limited_wait_ms(1, ""), 1000);
	EXPECT_EQ(ferry::rate_limited_wait_ms(2, "garbage"), 2000);
	EXPECT_EQ(ferry::rate_limited_wait_ms(0, "5s"), 500);			/* trailing junk */
	EXPECT_EQ(ferry::rate_limited_wait_ms(0, "-5"), 500);			/* negative */
	EXPECT_EQ(ferry::rate_limited_wait_ms(1, " 5 "), 5000);		/* OWS tolerated */
}

} // namespace
