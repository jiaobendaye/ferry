#ifndef FERRY_PLANNER_H
#define FERRY_PLANNER_H

namespace ferry
{

/*
 * Chunk layout for one download: the file maps to
 * ceil(file_size / chunk_size) fixed chunks, claimed dynamically by
 * the workers, so the number of active workers is min(jobs, chunk_count).
 */
struct ChunkPlan
{
	long long file_size = 0;
	long long chunk_size = 0;
	long long chunk_count = 0;		/* ceil(file_size / chunk_size); 0 when file_size <= 0 */
	int workers = 0;				/* min(jobs, chunk_count) */
};

ChunkPlan make_plan(long long file_size, long long chunk_size, int jobs);

/*
 * Byte offset / length of chunk idx within the file. The last chunk
 * may be shorter than chunk_size. Both return 0 for an out-of-range idx.
 */
long long chunk_offset(const ChunkPlan& plan, long long idx);
long long chunk_length(const ChunkPlan& plan, long long idx);

} // namespace ferry

#endif // FERRY_PLANNER_H
