#ifndef FERRY_CACHE_ADVISOR_H
#define FERRY_CACHE_ADVISOR_H

#include <functional>
#include <memory>
#include <sys/types.h>

#include "config/config.h"

namespace ferry
{

class Stats;

struct CacheAdviceRange
{
	off_t offset = 0;
	off_t length = 0;
	bool valid = false;
};

class CacheAdvisor
{
public:
	using AdviceFn = std::function<int(int, off_t, off_t, int)>;

	explicit CacheAdvisor(AdviceFn advise = AdviceFn(), long page_size = 0);

	void before_read(FileCachePolicy policy, int fd, long long offset,
					 long long length, Stats *stats) const;
	void after_read(FileCachePolicy policy, int fd, long long offset,
					long long bytes_read, Stats *stats) const;

	static CacheAdviceRange fully_covered_pages(long long offset,
											long long length,
											long page_size);

private:
	void advise(int fd, off_t offset, off_t length, int advice,
				Stats *stats) const;

	AdviceFn advise_;
	long page_size_;
};

std::shared_ptr<CacheAdvisor> default_cache_advisor();

} // namespace ferry

#endif // FERRY_CACHE_ADVISOR_H
