#include <gtest/gtest.h>
#include "range.h"

namespace
{

const long long SIZE = 1000;
const long long CAP = 8LL * 1024 * 1024;
const long long THRESHOLD = CAP;

ferry::RangeDecision decide(const std::string& header, long long size = SIZE,
						 long long cap = CAP, long long threshold = THRESHOLD)
{
	return ferry::decide_range(header, size, cap, threshold);
}

TEST(Range, WholeFileWithinThreshold)
{
	auto d = decide("");
	EXPECT_EQ(d.status, 200);
	EXPECT_EQ(d.offset, 0);
	EXPECT_EQ(d.length, SIZE);
}

TEST(Range, ThresholdBoundaryEqualsServesWhole)
{
	auto d = decide("", THRESHOLD);
	EXPECT_EQ(d.status, 200);
	EXPECT_EQ(d.length, THRESHOLD);
}

TEST(Range, NonRangeOverThresholdIs413)
{
	auto d = decide("", THRESHOLD + 1);
	EXPECT_EQ(d.status, 413);
}

TEST(Range, ExactRange)
{
	auto d = decide("bytes=0-99");
	EXPECT_EQ(d.status, 206);
	EXPECT_EQ(d.offset, 0);
	EXPECT_EQ(d.length, 100);
}

TEST(Range, MiddleRange)
{
	auto d = decide("bytes=400-599");
	EXPECT_EQ(d.status, 206);
	EXPECT_EQ(d.offset, 400);
	EXPECT_EQ(d.length, 200);
}

TEST(Range, LastClampedToEof)
{
	auto d = decide("bytes=900-5000");
	EXPECT_EQ(d.status, 206);
	EXPECT_EQ(d.offset, 900);
	EXPECT_EQ(d.length, 100);
}

TEST(Range, OpenEndedCapped)
{
	long long big = 10LL * 1024 * 1024 * 1024;	/* 10 GiB */
	long long cap = 8LL * 1024 * 1024;
	auto d = decide("bytes=0-", big, cap, cap);

	EXPECT_EQ(d.status, 206);
	EXPECT_EQ(d.offset, 0);
	EXPECT_EQ(d.length, cap);
}

TEST(Range, WideExplicitRangeCapped)
{
	long long big = 100LL * 1024 * 1024;
	long long cap = 8LL * 1024 * 1024;
	auto d = decide("bytes=1000-99999999", big, cap, cap);

	EXPECT_EQ(d.status, 206);
	EXPECT_EQ(d.offset, 1000);
	EXPECT_EQ(d.length, cap);			/* start preserved, length capped */
}

TEST(Range, RangeSmallerThanCapUntouched)
{
	auto d = decide("bytes=0-49");
	EXPECT_EQ(d.length, 50);
}

TEST(Range, SuffixWithinFile)
{
	auto d = decide("bytes=-100");
	EXPECT_EQ(d.status, 206);
	EXPECT_EQ(d.offset, 900);
	EXPECT_EQ(d.length, 100);
}

TEST(Range, SuffixLargerThanFile)
{
	auto d = decide("bytes=-5000");
	EXPECT_EQ(d.status, 206);
	EXPECT_EQ(d.offset, 0);
	EXPECT_EQ(d.length, SIZE);
}

TEST(Range, SuffixZeroUnsatisfiable)
{
	EXPECT_EQ(decide("bytes=-0").status, 416);
}

TEST(Range, StartPastEndIs416)
{
	EXPECT_EQ(decide("bytes=10000-").status, 416);
	EXPECT_EQ(decide("bytes=1000-1100").status, 416);	/* start == size */
}

TEST(Range, MultiRangeKeepsFirst)
{
	auto d = decide("bytes=0-99,200-299");
	EXPECT_EQ(d.status, 206);
	EXPECT_EQ(d.offset, 0);
	EXPECT_EQ(d.length, 100);
}

TEST(Range, UnsupportedUnitIgnored)
{
	EXPECT_EQ(decide("items=0-99").status, 200);
	auto d413 = decide("items=0-99", THRESHOLD + 1);
	EXPECT_EQ(d413.status, 413);		/* non-Range rules apply */
}

TEST(Range, SyntacticallyInvalidIgnored)
{
	EXPECT_EQ(decide("bytes=abc").status, 200);
	EXPECT_EQ(decide("bytes=5-1").status, 200);		/* first > last */
	EXPECT_EQ(decide("bytes=").status, 200);
	EXPECT_EQ(decide("nonsense").status, 200);
	EXPECT_EQ(decide("bytes=1-2-3").status, 200);
}

TEST(Range, UnitCaseInsensitive)
{
	auto d = decide("BYTES=0-99");
	EXPECT_EQ(d.status, 206);
	EXPECT_EQ(d.length, 100);
}

TEST(Range, WhitespaceTolerated)
{
	auto d = decide("  bytes = 0-99 ");
	EXPECT_EQ(d.status, 206);
	EXPECT_EQ(d.length, 100);
}

} // namespace
