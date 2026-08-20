#ifndef FERRY_MMAP_BODY_H
#define FERRY_MMAP_BODY_H

#include "file_body.h"

namespace ferry
{

class MmapBody final : public FileBody
{
public:
	~MmapBody() override;

	/* Returns nullptr without consuming fd when mapping cannot be used. */
	static MmapBody *try_start(protocol::HttpResponse *resp, int fd,
							   const FileBodySpec& spec, Stats *stats);

private:
	MmapBody(protocol::HttpResponse *resp, const FileBodySpec& spec,
			 void *mapping, size_t mapping_len, Stats *stats);

	void *mapping_;
	size_t mapping_len_;
	Stats *stats_;
};

} // namespace ferry

#endif // FERRY_MMAP_BODY_H
