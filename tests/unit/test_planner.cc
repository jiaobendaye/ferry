#include <gtest/gtest.h>
#include "planner.h"

namespace
{

const long long MB = 1024LL * 1024;

ferry::ChunkPlan plan(long long size, long long chunk, int jobs)
{
	return ferry::make_plan(size, chunk, jobs);
}

TEST(Planner, ZeroSizeGivesNoChunksAndNoWorkers)
{
	auto p = plan(0, 8 * MB, 4);
	EXPECT_EQ(p.file_size, 0);
	EXPECT_EQ(p.chunk_size, 8 * MB);
	EXPECT_EQ(p.chunk_count, 0);
	EXPECT_EQ(p.workers, 0);
}

TEST(Planner, NegativeSizeGivesNoChunks)
{
	auto p = plan(-1, 8 * MB, 4);
	EXPECT_EQ(p.chunk_count, 0);
	EXPECT_EQ(p.workers, 0);
}

TEST(Planner, SmallerThanChunkIsOneChunk)
{
	auto p = plan(1000, 8 * MB, 4);
	EXPECT_EQ(p.chunk_count, 1);
	EXPECT_EQ(p.workers, 1);			/* min(4, 1) */
	EXPECT_EQ(ferry::chunk_offset(p, 0), 0);
	EXPECT_EQ(ferry::chunk_length(p, 0), 1000);
}

TEST(Planner, ExactMultipleHasFullLastChunk)
{
	auto p = plan(3 * MB, MB, 4);
	EXPECT_EQ(p.chunk_count, 3);
	EXPECT_EQ(p.workers, 3);			/* min(4, 3) */
	EXPECT_EQ(ferry::chunk_offset(p, 2), 2 * MB);
	EXPECT_EQ(ferry::chunk_length(p, 2), MB);
}

TEST(Planner, BoundaryPlusOneAddsChunk)
{
	auto p = plan(3 * MB + 1, MB, 4);
	EXPECT_EQ(p.chunk_count, 4);
	EXPECT_EQ(ferry::chunk_offset(p, 3), 3 * MB);
	EXPECT_EQ(ferry::chunk_length(p, 3), 1);		/* short last chunk */
}

TEST(Planner, WorkersAreMinOfJobsAndChunks)
{
	/* spec scenario: 20 MiB / 8 MiB = 3 chunks, jobs 4 -> 3 workers */
	auto p = plan(20 * MB, 8 * MB, 4);
	EXPECT_EQ(p.chunk_count, 3);
	EXPECT_EQ(p.workers, 3);

	auto p2 = plan(100 * MB, MB, 2);
	EXPECT_EQ(p2.chunk_count, 100);
	EXPECT_EQ(p2.workers, 2);
}

TEST(Planner, OffsetsAndLengthsIncludingShortLastChunk)
{
	long long size = 10 * MB + 7;
	auto p = plan(size, 4 * MB, 4);
	ASSERT_EQ(p.chunk_count, 3);

	EXPECT_EQ(ferry::chunk_offset(p, 0), 0);
	EXPECT_EQ(ferry::chunk_length(p, 0), 4 * MB);
	EXPECT_EQ(ferry::chunk_offset(p, 1), 4 * MB);
	EXPECT_EQ(ferry::chunk_length(p, 1), 4 * MB);
	EXPECT_EQ(ferry::chunk_offset(p, 2), 8 * MB);
	EXPECT_EQ(ferry::chunk_length(p, 2), 2 * MB + 7);
}

TEST(Planner, ChunksTileTheWholeFile)
{
	long long size = 37 * MB + 13;
	auto p = plan(size, 5 * MB, 8);

	long long covered = 0;
	for (long long i = 0; i < p.chunk_count; i++)
	{
		EXPECT_EQ(ferry::chunk_offset(p, i), covered);
		EXPECT_LE(ferry::chunk_length(p, i), 5 * MB);
		EXPECT_GT(ferry::chunk_length(p, i), 0);
		covered += ferry::chunk_length(p, i);
	}
	EXPECT_EQ(covered, size);
}

TEST(Planner, OutOfRangeIndexReturnsZero)
{
	auto p = plan(20 * MB, 8 * MB, 4);

	EXPECT_EQ(ferry::chunk_offset(p, -1), 0);
	EXPECT_EQ(ferry::chunk_length(p, -1), 0);
	EXPECT_EQ(ferry::chunk_offset(p, 3), 0);		/* count is 3 */
	EXPECT_EQ(ferry::chunk_length(p, 3), 0);
	EXPECT_EQ(ferry::chunk_offset(p, 1000), 0);
	EXPECT_EQ(ferry::chunk_length(p, 1000), 0);
}

TEST(Planner, OutOfRangeIndexOnEmptyPlanReturnsZero)
{
	auto p = plan(0, 8 * MB, 4);
	EXPECT_EQ(ferry::chunk_offset(p, 0), 0);
	EXPECT_EQ(ferry::chunk_length(p, 0), 0);
}

} // namespace
