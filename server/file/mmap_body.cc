#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>

#include "mmap_body.h"
#include "observability/stats.h"

namespace ferry
{

MmapBody::MmapBody(protocol::HttpResponse *resp, const FileBodySpec& spec,
				   void *mapping, size_t mapping_len, Stats *stats)
	: FileBody(resp, spec),
	  mapping_(mapping),
	  mapping_len_(mapping_len),
	  stats_(stats)
{
}

MmapBody::~MmapBody()
{
	if (this->mapping_)
		munmap(this->mapping_, this->mapping_len_);
	if (this->stats_)
		this->stats_->mmap_close(this->served_);
}

MmapBody *MmapBody::try_start(protocol::HttpResponse *resp, int fd,
							  const FileBodySpec& spec, Stats *stats)
{
	if (spec.length == 0)
	{
		close(fd);
		MmapBody *body = new MmapBody(resp, spec, nullptr, 0, nullptr);
		body->set_response(nullptr, 0);
		return body;
	}

	long page_size = sysconf(_SC_PAGESIZE);
	off_t mapping_offset = (off_t)spec.offset;
	size_t delta = 0;
	bool representable = page_size > 0;

	if (representable)
	{
		mapping_offset -= mapping_offset % (off_t)page_size;
		delta = (size_t)((off_t)spec.offset - mapping_offset);
		representable = (unsigned long long)spec.length <=
							(unsigned long long)SIZE_MAX - delta;
	}

	if (!representable)
		return nullptr;

	size_t mapping_len = delta + (size_t)spec.length;
	void *mapping = mmap(nullptr, mapping_len, PROT_READ, MAP_PRIVATE, fd,
						 mapping_offset);
	if (mapping == MAP_FAILED)
		return nullptr;

	close(fd);
	MmapBody *body = new MmapBody(resp, spec, mapping, mapping_len, stats);
	body->set_response((const char *)mapping + delta, (size_t)spec.length);
	if (stats)
		stats->mmap_open(spec.length);
	return body;
}

} // namespace ferry
