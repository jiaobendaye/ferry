#ifndef FERRY_PREAD_BODY_H
#define FERRY_PREAD_BODY_H

#include "workflow/WFTaskFactory.h"

#include "file_body.h"
#include "cache_advisor.h"

namespace ferry
{

class PreadBody final : public FileBody
{
public:
	~PreadBody() override;

	static PreadBody *prepare(protocol::HttpResponse *resp, int fd,
							  const FileBodySpec& spec, Stats *stats,
							  std::shared_ptr<CacheAdvisor> advisor,
							  SubTask **task_out);

private:
	PreadBody(protocol::HttpResponse *resp, const FileBodySpec& spec,
			  void *storage, Stats *stats,
			  std::shared_ptr<CacheAdvisor> advisor);

	static void on_read(WFFileIOTask *task);

	void *storage_;
	Stats *stats_;
	std::shared_ptr<CacheAdvisor> advisor_;
};

} // namespace ferry

#endif // FERRY_PREAD_BODY_H
