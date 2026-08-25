#include <fcntl.h>
#include <climits>
#include <tuple>
#include <vector>
#include <gtest/gtest.h>

#include "file/cache_advisor.h"
#include "observability/stats.h"

namespace
{

struct AdviceCall
{
	int fd;
	off_t offset;
	off_t length;
	int advice;
};

TEST(CacheAdvisor, ComputesFullyCoveredPages)
{
	auto aligned = ferry::CacheAdvisor::fully_covered_pages(0, 8192, 4096);
	ASSERT_TRUE(aligned.valid);
	EXPECT_EQ(aligned.offset, 0);
	EXPECT_EQ(aligned.length, 8192);

	auto unaligned = ferry::CacheAdvisor::fully_covered_pages(1, 8192, 4096);
	ASSERT_TRUE(unaligned.valid);
	EXPECT_EQ(unaligned.offset, 4096);
	EXPECT_EQ(unaligned.length, 4096);

	EXPECT_FALSE(ferry::CacheAdvisor::fully_covered_pages(1, 4095, 4096).valid);
	EXPECT_FALSE(ferry::CacheAdvisor::fully_covered_pages(0, 0, 4096).valid);
	EXPECT_FALSE(ferry::CacheAdvisor::fully_covered_pages(-1, 8192, 4096).valid);
	EXPECT_FALSE(ferry::CacheAdvisor::fully_covered_pages(
		LLONG_MAX - 100, 200, 4096).valid);
}

TEST(CacheAdvisor, PoliciesCallExpectedAdviceAndCounters)
{
	std::vector<AdviceCall> calls;
	ferry::CacheAdvisor advisor(
		[&calls](int fd, off_t offset, off_t length, int advice) {
			calls.push_back({fd, offset, length, advice});
			return 0;
		}, 4096);
	ferry::Stats stats;

	advisor.before_read(ferry::FileCachePolicy::NORMAL, 7, 1, 8192, &stats);
	advisor.after_read(ferry::FileCachePolicy::NORMAL, 7, 1, 8192, &stats);
	EXPECT_TRUE(calls.empty());

	advisor.before_read(ferry::FileCachePolicy::NOREUSE, 7, 123, 456, &stats);
	ASSERT_EQ(calls.size(), 1u);
	EXPECT_EQ(calls[0].offset, 123);
	EXPECT_EQ(calls[0].length, 456);
	EXPECT_EQ(calls[0].advice, POSIX_FADV_NOREUSE);

	advisor.after_read(ferry::FileCachePolicy::DROP_AFTER_READ,
					   8, 1, 8192, &stats);
	ASSERT_EQ(calls.size(), 2u);
	EXPECT_EQ(calls[1].offset, 4096);
	EXPECT_EQ(calls[1].length, 4096);
	EXPECT_EQ(calls[1].advice, POSIX_FADV_DONTNEED);

	auto snapshot = stats.snapshot();
	EXPECT_EQ(snapshot.cache_advice_calls, 2);
	EXPECT_EQ(snapshot.cache_advice_bytes, 456 + 4096);
	EXPECT_EQ(snapshot.cache_advice_errors, 0);
}

TEST(CacheAdvisor, AdviceFailureIsCountedWithoutAcceptedBytes)
{
	ferry::CacheAdvisor advisor(
		[](int, off_t, off_t, int) { return EINVAL; }, 4096);
	ferry::Stats stats;

	advisor.after_read(ferry::FileCachePolicy::DROP_AFTER_READ,
					   9, 0, 4096, &stats);
	auto snapshot = stats.snapshot();
	EXPECT_EQ(snapshot.cache_advice_calls, 1);
	EXPECT_EQ(snapshot.cache_advice_bytes, 0);
	EXPECT_EQ(snapshot.cache_advice_errors, 1);
}

TEST(CacheAdvisor, EmptyShortAndReadFailureResultsDoNotAdvise)
{
	int calls = 0;
	ferry::CacheAdvisor advisor(
		[&calls](int, off_t, off_t, int) { calls++; return 0; }, 4096);
	ferry::Stats stats;

	advisor.before_read(ferry::FileCachePolicy::NOREUSE, 3, 0, 0, &stats);
	advisor.after_read(ferry::FileCachePolicy::DROP_AFTER_READ, 3, 1, 4095,
					   &stats);
	advisor.after_read(ferry::FileCachePolicy::DROP_AFTER_READ, 3, 0, -1,
					   &stats);
	EXPECT_EQ(calls, 0);
	EXPECT_EQ(stats.snapshot().cache_advice_calls, 0);
}

} // namespace
