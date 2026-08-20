#include <cstdio>
#include <string>

#include "workflow/HttpMessage.h"

#include "mmap_body.h"
#include "pread_body.h"
#include "observability/stats.h"

using protocol::HttpResponse;

namespace ferry
{

FileBody::FileBody(HttpResponse *resp, const FileBodySpec& spec)
	: resp_(resp), spec_(spec)
{
}

void FileBody::set_response(const void *body, size_t len)
{
	if (this->spec_.partial)
	{
		char content_range[128];
		snprintf(content_range, sizeof(content_range), "bytes %lld-%lld/%lld",
				 this->spec_.offset,
				 this->spec_.offset + (long long)len - 1,
				 this->spec_.file_size);
		this->resp_->set_status_code("206");
		this->resp_->set_header_pair("Content-Range", content_range);
	}
	else
		this->resp_->set_status_code("200");

	struct tm gmt;
	char date[64];
	gmtime_r(&this->spec_.mtime, &gmt);
	strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
	this->resp_->set_header_pair("Last-Modified", date);
	this->resp_->set_header_pair("Content-Length", std::to_string(len));
	if (len > 0)
		this->resp_->append_output_body_nocopy(body, len);
	this->served_ = (long long)len;
}

void FileBody::set_error()
{
	static const char body[] = "503 Internal Server Error";
	this->resp_->set_status_code("503");
	this->resp_->set_header_pair("Content-Length",
							 std::to_string(sizeof(body) - 1));
	this->resp_->append_output_body(body, sizeof(body) - 1);
}

PreparedFileBody prepare_file_body(HttpResponse *resp, int fd,
								   const FileBodySpec& spec,
								   FileBodyMode mode, Stats *stats)
{
	if (mode == FileBodyMode::MMAP)
	{
		MmapBody *body = MmapBody::try_start(resp, fd, spec, stats);
		if (body)
			return {body, nullptr};
		if (stats)
			stats->mmap_fallback();
	}

	SubTask *task = nullptr;
	PreadBody *body = PreadBody::prepare(resp, fd, spec, &task);
	return {body, task};
}

} // namespace ferry
