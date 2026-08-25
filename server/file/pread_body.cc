#include <cstdlib>
#include <unistd.h>

#include "workflow/WFTask.h"
#include "pread_body.h"

namespace ferry
{

PreadBody::PreadBody(protocol::HttpResponse *resp, const FileBodySpec& spec,
					 void *storage, Stats *stats,
					 std::shared_ptr<CacheAdvisor> advisor)
	: FileBody(resp, spec), storage_(storage), stats_(stats),
	  advisor_(std::move(advisor))
{
}

PreadBody::~PreadBody()
{
	free(this->storage_);
}

PreadBody *PreadBody::prepare(protocol::HttpResponse *resp, int fd,
							  const FileBodySpec& spec, Stats *stats,
							  std::shared_ptr<CacheAdvisor> advisor,
							  SubTask **task_out)
{
	*task_out = nullptr;
	void *storage = malloc(spec.length > 0 ? (size_t)spec.length : 1);
	if (!storage)
	{
		close(fd);
		PreadBody failed(resp, spec, nullptr, stats, std::move(advisor));
		failed.set_error();
		return nullptr;
	}

	PreadBody *body = new PreadBody(resp, spec, storage, stats,
										std::move(advisor));
	body->advisor_->before_read(spec.cache_policy, fd, spec.offset,
								 spec.length, stats);
	WFFileIOTask *task = WFTaskFactory::create_pread_task(
						fd, storage, (size_t)spec.length, (off_t)spec.offset,
						PreadBody::on_read);
	task->user_data = body;
	*task_out = task;
	return body;
}

void PreadBody::on_read(WFFileIOTask *task)
{
	FileIOArgs *args = task->get_args();
	long ret = task->get_retval();
	PreadBody *body = (PreadBody *)task->user_data;

	if (task->get_state() != WFT_STATE_SUCCESS || ret < 0)
	{
		close(args->fd);
		body->set_error();
		return;
	}

	body->advisor_->after_read(body->spec_.cache_policy, args->fd,
								body->spec_.offset, (long long)ret, body->stats_);
	close(args->fd);
	body->set_response(args->buf, (size_t)ret);
}

} // namespace ferry
