#include <fcntl.h>
#include <limits>
#include <unistd.h>

#include "cache_advisor.h"
#include "observability/stats.h"

namespace ferry
{

CacheAdvisor::CacheAdvisor(AdviceFn advise, long page_size)
	: advise_(std::move(advise)), page_size_(page_size)
{
	if (!this->advise_)
	{
		this->advise_ = [](int fd, off_t offset, off_t length, int advice) {
			return posix_fadvise(fd, offset, length, advice);
		};
	}
	if (this->page_size_ <= 0)
		this->page_size_ = sysconf(_SC_PAGESIZE);
}

CacheAdviceRange CacheAdvisor::fully_covered_pages(long long offset,
											 long long length,
											 long page_size)
{
	CacheAdviceRange range;
	if (offset < 0 || length <= 0 || page_size <= 0)
		return range;

	const unsigned long long max_off =
		(unsigned long long)std::numeric_limits<off_t>::max();
	const unsigned long long start = (unsigned long long)offset;
	const unsigned long long len = (unsigned long long)length;
	const unsigned long long page = (unsigned long long)page_size;
	if (start > max_off || len > max_off - start)
		return range;

	const unsigned long long end = start + len;
	const unsigned long long remainder = start % page;
	const unsigned long long aligned_start = remainder == 0 ? start :
		start + (page - remainder);
	const unsigned long long aligned_end = end - end % page;
	if (aligned_start > max_off || aligned_end <= aligned_start)
		return range;

	range.offset = (off_t)aligned_start;
	range.length = (off_t)(aligned_end - aligned_start);
	range.valid = true;
	return range;
}

void CacheAdvisor::advise(int fd, off_t offset, off_t length, int advice,
						  Stats *stats) const
{
	if (length <= 0)
		return;
	int rc = this->advise_(fd, offset, length, advice);
	if (stats)
		stats->record_cache_advice(rc == 0 ? (long long)length : 0, rc != 0);
}

void CacheAdvisor::before_read(FileCachePolicy policy, int fd,
								long long offset, long long length,
								Stats *stats) const
{
	if (policy != FileCachePolicy::NOREUSE || offset < 0 || length <= 0)
		return;
	if ((unsigned long long)offset >
		(unsigned long long)std::numeric_limits<off_t>::max() ||
		(unsigned long long)length >
		(unsigned long long)std::numeric_limits<off_t>::max())
		return;
	this->advise(fd, (off_t)offset, (off_t)length, POSIX_FADV_NOREUSE, stats);
}

void CacheAdvisor::after_read(FileCachePolicy policy, int fd,
							   long long offset, long long bytes_read,
							   Stats *stats) const
{
	if (policy != FileCachePolicy::DROP_AFTER_READ)
		return;
	CacheAdviceRange range = fully_covered_pages(offset, bytes_read,
												 this->page_size_);
	if (range.valid)
		this->advise(fd, range.offset, range.length, POSIX_FADV_DONTNEED, stats);
}

std::shared_ptr<CacheAdvisor> default_cache_advisor()
{
	static std::shared_ptr<CacheAdvisor> advisor =
		std::make_shared<CacheAdvisor>();
	return advisor;
}

} // namespace ferry
