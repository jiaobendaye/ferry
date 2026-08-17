#include <algorithm>
#include "planner.h"

namespace ferry
{

ChunkPlan make_plan(long long file_size, long long chunk_size, int jobs)
{
	ChunkPlan plan;
	plan.file_size = file_size;
	plan.chunk_size = chunk_size;

	/* chunk_size <= 0 is rejected by the CLI; guard against division by zero. */
	if (file_size <= 0 || chunk_size <= 0)
	{
		plan.chunk_count = 0;
		plan.workers = 0;
		return plan;
	}

	plan.chunk_count = (file_size + chunk_size - 1) / chunk_size;
	plan.workers = (int)std::min((long long)jobs, plan.chunk_count);
	if (plan.workers < 0)
		plan.workers = 0;
	return plan;
}

long long chunk_offset(const ChunkPlan& plan, long long idx)
{
	if (idx < 0 || idx >= plan.chunk_count)
		return 0;
	return idx * plan.chunk_size;
}

long long chunk_length(const ChunkPlan& plan, long long idx)
{
	if (idx < 0 || idx >= plan.chunk_count)
		return 0;

	long long offset = idx * plan.chunk_size;
	long long rest = plan.file_size - offset;
	return rest < plan.chunk_size ? rest : plan.chunk_size;
}

} // namespace ferry
