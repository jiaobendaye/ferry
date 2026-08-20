#include <cstdlib>
#include <unistd.h>

#include "workflow/WFTask.h"
#include "pread_body.h"

namespace ferry
{

PreadBody::PreadBody(protocol::HttpResponse *resp, const FileBodySpec& spec,
					 void *storage)
	: FileBody(resp, spec), storage_(storage)
{
}

PreadBody::~PreadBody()
{
	free(this->storage_);
}

PreadBody *PreadBody::prepare(protocol::HttpResponse *resp, int fd,
							  const FileBodySpec& spec, SubTask **task_out)
{
	*task_out = nullptr;
	void *storage = malloc(spec.length > 0 ? (size_t)spec.length : 1);
	if (!storage)
	{
		close(fd);
		PreadBody failed(resp, spec, nullptr);
		failed.set_error();
		return nullptr;
	}

	PreadBody *body = new PreadBody(resp, spec, storage);
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

	close(args->fd);

	if (task->get_state() != WFT_STATE_SUCCESS || ret < 0)
	{
		body->set_error();
		return;
	}

	body->set_response(args->buf, (size_t)ret);
}

} // namespace ferry
