#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTask.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/Workflow.h"

#include "handler.h"
#include "range.h"

using protocol::HttpRequest;
using protocol::HttpResponse;
using protocol::HttpHeaderCursor;

namespace ferry
{

/* ---------------- path safety ---------------- */

static bool hex_digit(char c, int *v)
{
	if (c >= '0' && c <= '9')
		*v = c - '0';
	else if (c >= 'a' && c <= 'f')
		*v = c - 'a' + 10;
	else if (c >= 'A' && c <= 'F')
		*v = c - 'A' + 10;
	else
		return false;
	return true;
}

static bool percent_decode(const std::string& in, std::string *out)
{
	out->clear();
	for (size_t i = 0; i < in.size(); i++)
	{
		char c = in[i];
		if (c == '%')
		{
			int hi, lo;
			if (i + 2 >= in.size() ||
				!hex_digit(in[i + 1], &hi) || !hex_digit(in[i + 2], &lo))
				return false;
			out->push_back((char)((hi << 4) | lo));
			i += 2;
		}
		else
			out->push_back(c);
	}
	return true;
}

bool uri_to_safe_path(const std::string& root, const char *uri,
					  std::string *out)
{
	std::string path_part(uri);
	size_t q = path_part.find_first_of("?#");
	if (q != std::string::npos)
		path_part.erase(q);

	std::string decoded;
	if (!percent_decode(path_part, &decoded))
		return false;
	if (decoded.find('\0') != std::string::npos)
		return false;

	std::vector<std::string> segs;
	size_t pos = 0;

	while (pos <= decoded.size())
	{
		size_t slash = decoded.find('/', pos);
		size_t end = (slash == std::string::npos ? decoded.size() : slash);
		std::string seg = decoded.substr(pos, end - pos);

		if (seg == "..")
		{
			if (segs.empty())
				return false;
			segs.pop_back();
		}
		else if (!seg.empty() && seg != ".")
			segs.push_back(seg);

		if (slash == std::string::npos)
			break;
		pos = slash + 1;
	}

	std::string clean;
	for (const std::string& s : segs)
		clean += "/" + s;

	*out = root + clean;
	return true;
}

/* ---------------- http helpers ---------------- */

static std::string http_date(time_t t)
{
	struct tm gmt;
	char buf[64];

	gmtime_r(&t, &gmt);
	strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
	return buf;
}

static void set_text_body(HttpResponse *resp, const std::string& body)
{
	resp->set_header_pair("Content-Length", std::to_string(body.size()));
	if (!body.empty())
		resp->append_output_body(body.c_str(), body.size());
}

static void reply_error(HttpResponse *resp, const char *code,
						const std::string& body)
{
	resp->set_status_code(code);
	set_text_body(resp, body);
}

/*
 * workflow's HttpHeaderCursor is forward-only and does not rewind after
 * find(), so each lookup uses a fresh cursor.
 */
static bool find_header(const HttpRequest *req, const char *name,
						std::string *value)
{
	HttpHeaderCursor cursor(req);
	return cursor.find(name, *value);
}

/*
 * If-Range v1: Last-Modified dates only. An entity-tag validator or an
 * unparseable date counts as "does not match" -> full-representation
 * semantics (RFC 9110 13.1.2).
 */
static bool if_range_matches(const std::string& if_range, time_t mtime)
{
	if (if_range.find('"') != std::string::npos)
		return false;

	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	if (!strptime(if_range.c_str(), "%a, %d %b %Y %H:%M:%S", &tm))
		return false;

	return timegm(&tm) == mtime;
}

/* ---------------- file body path ---------------- */

struct FileContext
{
	HttpResponse *resp;
	void *buf;
	long long offset;
	long long file_size;
	time_t mtime;
	bool partial;					/* 206 vs 200 */
};

static void pread_callback(WFFileIOTask *task)
{
	FileIOArgs *args = task->get_args();
	long ret = task->get_retval();
	FileContext *ctx = (FileContext *)task->user_data;
	HttpResponse *resp = ctx->resp;

	close(args->fd);

	if (task->get_state() != WFT_STATE_SUCCESS || ret < 0)
	{
		resp->set_status_code("503");
		set_text_body(resp, "503 Internal Server Error");
		return;
	}

	if (ctx->partial)
	{
		char cr[128];
		snprintf(cr, sizeof(cr), "bytes %lld-%lld/%lld",
				 ctx->offset, ctx->offset + ret - 1, ctx->file_size);
		resp->set_status_code("206");
		resp->set_header_pair("Content-Range", cr);
	}
	else
		resp->set_status_code("200");

	resp->set_header_pair("Last-Modified", http_date(ctx->mtime));
	resp->set_header_pair("Content-Length", std::to_string(ret));
	resp->append_output_body_nocopy(args->buf, ret);
}

/* ---------------- handler ---------------- */

Handler::Handler(const ServerConfig& cfg, std::shared_ptr<Acl> acl,
				 std::shared_ptr<RateLimiter> limiter)
	: cfg_(cfg), acl_(std::move(acl)), limiter_(std::move(limiter))
{
	while (this->cfg_.root.size() > 1 && this->cfg_.root.back() == '/')
		this->cfg_.root.pop_back();
}

void Handler::process(WFHttpTask *server_task)
{
	HttpRequest *req = server_task->get_req();
	HttpResponse *resp = server_task->get_resp();
	const char *method = req->get_method();

	resp->add_header_pair("Accept-Ranges", "bytes");
	resp->add_header_pair("Server", "ferry-server");

	if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
	{
		resp->set_header_pair("Allow", "GET, HEAD");
		reply_error(resp, "405", "405 Method Not Allowed");
		return;
	}

	/* ---- client IP ---- */
	std::string xff;
	find_header(req, "X-Forwarded-For", &xff);

	struct sockaddr_storage ss;
	socklen_t sslen = sizeof(ss);
	const struct sockaddr *peer = NULL;

	if (server_task->get_peer_addr((struct sockaddr *)&ss, &sslen) == 0)
		peer = (const struct sockaddr *)&ss;

	IpAddr ip;
	if (!resolve_client_ip(xff, this->cfg_.trust_hops, peer, sslen, &ip))
	{
		reply_error(resp, "400", "400 Bad Request");
		return;
	}

	/* ---- ACL (blacklist priority) ---- */
	if (this->acl_ && !this->acl_->allowed(ip))
	{
		reply_error(resp, "403", "403 Forbidden");
		return;
	}

	/* ---- path safety ---- */
	std::string path;
	if (!uri_to_safe_path(this->cfg_.root, req->get_request_uri(), &path))
	{
		reply_error(resp, "400", "400 Bad Request");
		return;
	}

	struct stat st;
	if (stat(path.c_str(), &st) < 0 || !S_ISREG(st.st_mode))
	{
		reply_error(resp, "404", "404 Not Found");
		return;
	}

	/* ---- HEAD: headers only, full size, no charge ---- */
	if (strcmp(method, "HEAD") == 0)
	{
		resp->set_status_code("200");
		resp->set_header_pair("Last-Modified", http_date(st.st_mtime));
		resp->set_header_pair("Content-Length", std::to_string(st.st_size));
		return;
	}

	/* ---- Range decision (If-Range may invalidate it) ---- */
	std::string range_value;
	std::string if_range;

	if (find_header(req, "If-Range", &if_range) &&
		!if_range_matches(if_range, st.st_mtime))
	{
		/* stale validator: process as non-Range */
	}
	else
		find_header(req, "Range", &range_value);

	RangeDecision d = decide_range(range_value, st.st_size,
								   this->cfg_.cap_bytes,
								   this->cfg_.threshold());

	if (d.status == 413)
	{
		std::string body = "413 Content Too Large: file size " +
			std::to_string(st.st_size) +
			" exceeds the single-response limit " +
			std::to_string(this->cfg_.threshold()) +
			" bytes; use HTTP Range requests to download it in chunks.";
		reply_error(resp, "413", body);
		return;
	}

	if (d.status == 416)
	{
		resp->set_header_pair("Content-Range",
							  "bytes */" + std::to_string(st.st_size));
		reply_error(resp, "416", "416 Range Not Satisfiable");
		return;
	}

	/* ---- rate limiter ---- */
	RateLimiter::Verdict verdict;

	if (this->limiter_ && !this->limiter_->disabled())
		verdict = this->limiter_->reserve(ip.to_string(), d.length);

	if (verdict.rejected)
	{
		resp->set_header_pair("Retry-After",
							  std::to_string(this->cfg_.max_wait_sec));
		reply_error(resp, "429", "429 Too Many Requests");
		return;
	}

	/* ---- async file read ---- */
	int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
	{
		reply_error(resp, "404", "404 Not Found");
		return;
	}

	void *buf = malloc(d.length > 0 ? d.length : 1);
	if (!buf)
	{
		close(fd);
		reply_error(resp, "503", "503 Internal Server Error");
		return;
	}

	FileContext *ctx = new FileContext();
	ctx->resp = resp;
	ctx->buf = buf;
	ctx->offset = d.offset;
	ctx->file_size = st.st_size;
	ctx->mtime = st.st_mtime;
	ctx->partial = (d.status == 206);

	WFFileIOTask *pread_task = WFTaskFactory::create_pread_task(
						fd, buf, (size_t)d.length, (off_t)d.offset,
						pread_callback);
	pread_task->user_data = ctx;

	server_task->user_data = ctx;
	server_task->set_callback([](WFHttpTask *task) {
		FileContext *c = (FileContext *)task->user_data;
		free(c->buf);
		delete c;
	});

	SeriesWork *series = series_of(server_task);

	if (verdict.wait.count() > 0)
	{
		long long ms = verdict.wait.count();
		WFTimerTask *timer = WFTaskFactory::create_timer_task(
						(unsigned int)(ms / 1000),
						(unsigned int)((ms % 1000) * 1000000), nullptr);
		series->push_back(timer);
	}

	series->push_back(pread_task);
}

} // namespace ferry
