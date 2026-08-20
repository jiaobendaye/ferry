#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <cstring>
#include <string>

#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTask.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/Workflow.h"

#include "handler.h"
#include "path.h"
#include "range.h"
#include "file/file_body.h"

using protocol::HttpRequest;
using protocol::HttpResponse;
using protocol::HttpHeaderCursor;

namespace ferry
{

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

static void reply_gate_reject(HttpResponse *resp, const ChainResult& r)
{
	resp->set_status_code(r.status);
	if (r.retry_after_sec > 0)
		resp->set_header_pair("Retry-After",
							  std::to_string(r.retry_after_sec));

	std::string body = r.status;
	body += (strcmp(r.status, "429") == 0) ? " Too Many Requests"
										   : " Service Unavailable";
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

/*
 * Per-request state anchored to server_task->user_data; the unified
 * completion callback is the single point where gate resources are
 * released, file bodies freed/unmapped, and the final status recorded.
 */
struct RequestState
{
	Stats *stats = nullptr;
	FileBody *body = nullptr;
	bool counted_inflight = false;
	ReleaseList releases;
};

static void on_request_complete(WFHttpTask *task)
{
	RequestState *rs = (RequestState *)task->user_data;

	if (!rs)
		return;

	long long served = rs->body ? rs->body->served() : 0;
	delete rs->body;

	if (rs->stats)
	{
		rs->stats->record_request(task->get_resp()->get_status_code(),
								  served);
		if (rs->counted_inflight)
			rs->stats->inflight_dec();
	}

	delete rs;						/* releases run in reverse order */
}

/* ---------------- handler ---------------- */

Handler::Handler(const ServerConfig& cfg, std::shared_ptr<Acl> acl,
				 std::shared_ptr<GateChain> pre_chain,
				 std::shared_ptr<GateChain> post_chain,
				 std::shared_ptr<Stats> stats)
	: cfg_(cfg),
	  acl_(std::move(acl)),
	  pre_chain_(std::move(pre_chain)),
	  post_chain_(std::move(post_chain)),
	  stats_(std::move(stats))
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

	/* unified completion hook: every path, sync and async */
	RequestState *rs = new RequestState();
	rs->stats = this->stats_.get();
	server_task->user_data = rs;
	server_task->set_callback(on_request_complete);

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

	std::string ip_key = ip.to_string();

	/* ---- ACL (blacklist priority) ---- */
	if (this->acl_ && !this->acl_->allowed(ip))
	{
		reply_error(resp, "403", "403 Forbidden");
		return;
	}

	/* ---- pre-chain gates: QPS + concurrency (global then per-IP) ---- */
	GateCtx gctx;
	gctx.ip_key = &ip_key;

	ChainResult pre = this->pre_chain_->run(gctx, this->stats_.get());
	if (pre.rejected)
	{
		reply_gate_reject(resp, pre);
		return;						/* nothing acquired yet to release */
	}
	rs->releases.append(std::move(pre.releases));
	if (this->stats_)
	{
		this->stats_->inflight_inc();
		rs->counted_inflight = true;
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

	/* ---- HEAD: headers only, full size, no bandwidth charge ---- */
	if (strcmp(method, "HEAD") == 0)
	{
		resp->set_status_code("200");
		resp->set_header_pair("Last-Modified", http_date(st.st_mtime));
		resp->set_header_pair("Content-Length", std::to_string(st.st_size));

		/* pre-chain shaping still applies to HEAD (e.g. QPS gates) */
		if (pre.delay.count() > 0)
		{
			long long ms = pre.delay.count();
			WFTimerTask *timer = WFTaskFactory::create_timer_task(
							(unsigned int)(ms / 1000),
							(unsigned int)((ms % 1000) * 1000000), nullptr);
			series_of(server_task)->push_back(timer);
		}
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

	/* ---- post-chain gates: bandwidth (global then per-IP) ---- */
	gctx.bytes = d.length;

	ChainResult post = this->post_chain_->run(gctx, this->stats_.get());
	if (post.rejected)
	{
		reply_gate_reject(resp, post);
		return;						/* pre-chain releases: RAII at callback */
	}
	rs->releases.append(std::move(post.releases));

	/* ---- file body setup ---- */
	int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		reply_error(resp, "404", "404 Not Found");
		return;
	}

	/* Bind the earlier Range decision to the same file description used by
	   pread/mmap. This closes the stat/open replacement window; mmap mode
	   still requires files not to be truncated after this point. */
	struct stat opened_st;
	if (fstat(fd, &opened_st) < 0 || !S_ISREG(opened_st.st_mode) ||
		opened_st.st_dev != st.st_dev || opened_st.st_ino != st.st_ino ||
		opened_st.st_size != st.st_size ||
		opened_st.st_mtim.tv_sec != st.st_mtim.tv_sec ||
		opened_st.st_mtim.tv_nsec != st.st_mtim.tv_nsec)
	{
		close(fd);
		reply_error(resp, "503", "503 File Changed During Request");
		return;
	}

	SeriesWork *series = series_of(server_task);

	/* composed shaping delay: max within each chain, sum across the
	   pre/post split (both waits must elapse) */
	long long total_ms = pre.delay.count() + post.delay.count();
	auto push_delay = [series, total_ms]() {
		if (total_ms > 0)
		{
			WFTimerTask *timer = WFTaskFactory::create_timer_task(
							(unsigned int)(total_ms / 1000),
							(unsigned int)((total_ms % 1000) * 1000000), nullptr);
			series->push_back(timer);
		}
	};

	FileBodySpec body_spec;
	body_spec.offset = d.offset;
	body_spec.length = d.length;
	body_spec.file_size = st.st_size;
	body_spec.mtime = st.st_mtime;
	body_spec.partial = (d.status == 206);
	PreparedFileBody prepared = prepare_file_body(
						resp, fd, body_spec, this->cfg_.file_body_mode,
						this->stats_.get());
	if (!prepared.body)
		return;

	rs->body = prepared.body;
	push_delay();
	if (prepared.task)
		series->push_back(prepared.task);
}

} // namespace ferry
