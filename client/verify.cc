#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>
#include <string>

#include <openssl/evp.h>

#include "workflow/WFTask.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/WFFacilities.h"
#include "workflow/Workflow.h"

#include "verify.h"

namespace ferry
{

struct Sha256State
{
	EVP_MD_CTX *md_ctx = NULL;
	int fd = -1;
	long long remaining = 0;		/* pread callbacks still to run */
	bool error = false;
	std::string digest;				/* lowercase hex, set on success */
	WFFacilities::WaitGroup *wait_group = NULL;
};

static std::string hex_encode(const unsigned char *data, unsigned int len)
{
	static const char digits[] = "0123456789abcdef";
	std::string out;

	out.resize(len * 2);
	for (unsigned int i = 0; i < len; i++)
	{
		out[2 * i] = digits[data[i] >> 4];
		out[2 * i + 1] = digits[data[i] & 0xf];
	}

	return out;
}

/*
 * One per chunk. The series executes the pread tasks in order, so all
 * callbacks share the EVP context safely. Each task owns its buffer via
 * user_data; it is freed here after the digest update.
 */
static void sha256_pread_callback(WFFileIOTask *task)
{
	Sha256State *state = (Sha256State *)series_of(task)->get_context();
	FileIOArgs *args = task->get_args();
	long ret = task->get_retval();

	if (task->get_state() != WFT_STATE_SUCCESS || ret < 0)
		state->error = true;
	else if (!state->error && ret > 0)
	{
		if (EVP_DigestUpdate(state->md_ctx, args->buf, (size_t)ret) != 1)
			state->error = true;
	}

	free(task->user_data);

	if (--state->remaining == 0)
	{
		if (!state->error)
		{
			unsigned char md[EVP_MAX_MD_SIZE];
			unsigned int md_len = 0;

			if (EVP_DigestFinal_ex(state->md_ctx, md, &md_len) == 1)
				state->digest = hex_encode(md, md_len);
		}

		state->wait_group->done();
	}
}

std::string sha256_of_file(const std::string& path, long long chunk_bytes)
{
	if (chunk_bytes <= 0)
		chunk_bytes = 8LL * 1024 * 1024;

	int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return "";

	struct stat st;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode))
	{
		close(fd);
		return "";
	}

	long long size = (long long)st.st_size;

	EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
	if (!md_ctx || EVP_DigestInit_ex(md_ctx, EVP_sha256(), NULL) != 1)
	{
		EVP_MD_CTX_free(md_ctx);
		close(fd);
		return "";
	}

	/* empty file: no IO tasks needed */
	if (size == 0)
	{
		unsigned char md[EVP_MAX_MD_SIZE];
		unsigned int md_len = 0;
		std::string digest;

		if (EVP_DigestFinal_ex(md_ctx, md, &md_len) == 1)
			digest = hex_encode(md, md_len);

		EVP_MD_CTX_free(md_ctx);
		close(fd);
		return digest;
	}

	long long n_chunks = size / chunk_bytes + (size % chunk_bytes != 0);

	Sha256State *state = new Sha256State();
	state->md_ctx = md_ctx;
	state->fd = fd;
	state->remaining = n_chunks;

	WFFacilities::WaitGroup wait_group(1);
	state->wait_group = &wait_group;

	SeriesWork *series = NULL;

	for (long long i = 0; i < n_chunks; i++)
	{
		long long offset = i * chunk_bytes;
		long long count = chunk_bytes;

		if (count > size - offset)
			count = size - offset;

		void *buf = malloc((size_t)count);
		WFFileIOTask *task = NULL;

		if (buf)
			task = WFTaskFactory::create_pread_task(fd, buf, (size_t)count,
													(off_t)offset,
													sha256_pread_callback);
		if (!task)
		{
			/*
			 * Allocation failure mid-build: drain the tasks already in the
			 * series so their buffers get freed, then report failure.
			 */
			free(buf);
			state->error = true;
			state->remaining = i;
			break;
		}

		task->user_data = buf;

		if (!series)
			series = Workflow::create_series_work(task, nullptr);
		else
			series->push_back(task);
	}

	if (!series)
	{
		/* not a single task was built */
		delete state;
		EVP_MD_CTX_free(md_ctx);
		close(fd);
		return "";
	}

	series->set_context(state);
	series->start();
	wait_group.wait();

	/*
	 * All task callbacks have finished before wait() returned, so the
	 * digest, the EVP context and the fd can be released here.
	 */
	std::string digest = state->digest;

	delete state;
	EVP_MD_CTX_free(md_ctx);
	close(fd);
	return digest;
}

static char to_lower(char c)
{
	if (c >= 'A' && c <= 'Z')
		c = c - 'A' + 'a';
	return c;
}

bool checksum_spec_matches(const std::string& spec,
						   const std::string& digest_hex)
{
	static const char PREFIX[] = "sha-256=";
	const size_t prefix_len = sizeof(PREFIX) - 1;

	if (spec.size() <= prefix_len)
		return false;

	for (size_t i = 0; i < prefix_len; i++)
	{
		if (to_lower(spec[i]) != PREFIX[i])
			return false;
	}

	std::string hex = spec.substr(prefix_len);
	if (hex.size() != digest_hex.size())
		return false;

	for (size_t i = 0; i < hex.size(); i++)
	{
		if (to_lower(hex[i]) != to_lower(digest_hex[i]))
			return false;
	}

	return true;
}

} // namespace ferry
