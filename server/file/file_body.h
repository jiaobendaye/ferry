#ifndef FERRY_FILE_BODY_H
#define FERRY_FILE_BODY_H

#include <cstddef>
#include <ctime>
#include <memory>

#include "config/config.h"

class SubTask;

namespace protocol
{
class HttpResponse;
}

namespace ferry
{

class Stats;
class CacheAdvisor;

struct FileBodySpec
{
	long long offset;
	long long length;
	long long file_size;
	time_t mtime;
	bool partial;
	FileCachePolicy cache_policy = FileCachePolicy::NORMAL;
};

/*
 * Owns the response storage until the HTTP task completes. Implementations
 * either free an asynchronous pread buffer or unmap mmap-backed pages.
 */
class FileBody
{
public:
	FileBody(protocol::HttpResponse *resp, const FileBodySpec& spec);
	virtual ~FileBody() = default;

	long long served() const { return this->served_; }

protected:
	void set_response(const void *body, size_t len);
	void set_error();

	protocol::HttpResponse *resp_;
	FileBodySpec spec_;
	long long served_ = 0;
};

struct PreparedFileBody
{
	FileBody *body;
	SubTask *task;
};

/*
 * Takes ownership of `fd`. mmap mode falls back to asynchronous pread when
 * mapping is unavailable. The caller appends a non-null task after any
 * shaping timer. `body` must be deleted at HTTP request completion; a null
 * body means an error response was installed synchronously.
 */
PreparedFileBody prepare_file_body(protocol::HttpResponse *resp, int fd,
								   const FileBodySpec& spec,
								   FileBodyMode mode, Stats *stats,
								   std::shared_ptr<CacheAdvisor> advisor = nullptr);

} // namespace ferry

#endif // FERRY_FILE_BODY_H
