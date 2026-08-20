#include <fcntl.h>
#include <signal.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFFacilities.h"
#include "workflow/WFTask.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/Workflow.h"

#include "backoff.h"
#include "resume/bitmap.h"
#include "engine.h"
#include "planner.h"
#include "http/probe.h"
#include "ui/progress.h"

namespace ferry
{

/* ---------------- small helpers ---------------- */

static bool resp_header(WFHttpTask *task, const char *name, std::string *value)
{
	protocol::HttpHeaderCursor cursor(task->get_resp());
	return cursor.find(name, *value);
}

static bool resp_body(WFHttpTask *task, std::string *out)
{
	protocol::HttpResponse *resp = task->get_resp();

	if (resp->is_chunked())
	{
		*out = protocol::HttpUtil::decode_chunked_body(resp);
		return true;
	}

	const void *body;
	size_t size;
	if (resp->get_parsed_body(&body, &size))
	{
		out->assign((const char *)body, size);
		return true;
	}

	out->clear();
	return true;
}

/* Parse "bytes S-E/T" (206 bodies). Returns false on malformed input. */
static bool parse_content_range(const std::string& v,
								long long *start, long long *end,
								long long *total)
{
	if (v.compare(0, 6, "bytes ") != 0)
		return false;

	const char *p = v.c_str() + 6;
	char *endp;

	*start = strtoll(p, &endp, 10);
	if (endp == p || *endp != '-')
		return false;

	p = endp + 1;
	*end = strtoll(p, &endp, 10);
	if (endp == p || *endp != '/')
		return false;

	p = endp + 1;
	if (*p == '*')
	{
		*total = -1;
		return true;
	}

	*total = strtoll(p, &endp, 10);
	if (endp == p || *endp != '\0')
		return false;

	return true;
}

/* ---------------- probe requests (task 3.2) ---------------- */

struct HeadFetch
{
	bool got_response = false;
	int status = 0;
	long long content_length = -1;
	bool accept_ranges = false;
	std::string last_modified;
};

static HeadFetch fetch_head(const ClientConfig& cfg)
{
	HeadFetch h;
	WFFacilities::WaitGroup wg(1);

	WFHttpTask *task = WFTaskFactory::create_http_task(cfg.url, 5, 0,
			[&h, &wg](WFHttpTask *task) {
		if (task->get_state() == WFT_STATE_SUCCESS)
		{
			protocol::HttpResponse *resp = task->get_resp();
			h.got_response = true;
			h.status = atoi(resp->get_status_code());

			/* fresh cursor per lookup: find() is forward-only */
			std::string v;
			if (resp_header(task, "Content-Length", &v))
				h.content_length = atoll(v.c_str());
			if (resp_header(task, "Accept-Ranges", &v) &&
				strcasecmp(v.c_str(), "bytes") == 0)
				h.accept_ranges = true;
			resp_header(task, "Last-Modified", &h.last_modified);
		}
		wg.done();
	});

	task->get_req()->set_method("HEAD");
	task->set_receive_timeout(cfg.receive_timeout_sec * 1000);
	task->start();
	wg.wait();
	return h;
}

struct ProbeFetch
{
	bool got_response = false;
	int status = 0;
	long long content_length = -1;	/* total size for 206, CL for 200 */
	std::string body;
};

static ProbeFetch fetch_probe(const ClientConfig& cfg)
{
	ProbeFetch p;
	WFFacilities::WaitGroup wg(1);

	WFHttpTask *task = WFTaskFactory::create_http_task(cfg.url, 5, 0,
			[&p, &wg](WFHttpTask *task) {
		if (task->get_state() == WFT_STATE_SUCCESS)
		{
			protocol::HttpResponse *resp = task->get_resp();
			p.got_response = true;
			p.status = atoi(resp->get_status_code());

			if (p.status == 206)
			{
				std::string cr;
				long long s, e, total;
				if (resp_header(task, "Content-Range", &cr) &&
					parse_content_range(cr, &s, &e, &total))
					p.content_length = total;
			}
			else
			{
				std::string v;
				if (resp_header(task, "Content-Length", &v))
					p.content_length = atoll(v.c_str());
			}

			resp_body(task, &p.body);
		}
		wg.done();
	});

	task->get_req()->add_header_pair("Range", "bytes=0-0");
	task->get_req()->set_size_limit(cfg.single_stream_limit + 65536);
	task->set_receive_timeout(cfg.receive_timeout_sec * 1000);
	task->start();
	wg.wait();
	return p;
}

/* ---------------- engine state ---------------- */

struct EngineState
{
	explicit EngineState(int workers) : wg(workers) { }

	ClientConfig cfg;
	std::string part_path;
	std::string meta_path;
	ChunkPlan plan;

	std::atomic<long long> next_claim{0};
	std::atomic<bool> stop{false};
	std::atomic<bool> sigint_received{false};
	std::atomic<long long> bytes_done{0};
	std::atomic<long long> chunks_done{0};
	std::atomic<long long> retries{0};

	std::mutex meta_mu;				/* guards meta + bitmap */
	DownloadMeta meta;

	std::mutex err_mu;
	std::string error;

	WFFacilities::WaitGroup wg;
};

static EngineState *g_state = NULL;

static void sigint_handler(int signo)
{
	if (g_state)
	{
		g_state->stop.store(true);
		g_state->sigint_received.store(true);
	}
}

static long long claim_chunk(EngineState *st)
{
	long long count = st->plan.chunk_count;

	while (true)
	{
		if (st->stop.load())
			return -1;

		long long idx = st->next_claim.fetch_add(1);
		if (idx >= count)
			return -1;

		std::lock_guard<std::mutex> lock(st->meta_mu);
		if (!st->meta.bitmap.test(idx))
			return idx;
	}
}

static void mark_done(EngineState *st, long long idx)
{
	std::lock_guard<std::mutex> lock(st->meta_mu);
	st->meta.bitmap.mark(idx);
	if (!st->meta_path.empty())
		save_meta_atomic(st->meta_path, st->meta);
}

static void fail_all(EngineState *st, const std::string& msg)
{
	{
		std::lock_guard<std::mutex> lock(st->err_mu);
		if (st->error.empty())
			st->error = msg;
	}
	st->stop.store(true);
}

/* ---------------- chunk worker chain ---------------- */

struct ChunkJob
{
	EngineState *st;
	long long idx;
	int attempt;
	int fd;							/* worker-owned part-file fd */
};

static void chunk_response(ChunkJob *job, WFHttpTask *task);

static WFHttpTask *make_chunk_http(ChunkJob *job)
{
	EngineState *st = job->st;
	long long off = chunk_offset(st->plan, job->idx);
	long long len = chunk_length(st->plan, job->idx);
	char range[96];

	snprintf(range, sizeof(range), "bytes=%lld-%lld", off, off + len - 1);

	WFHttpTask *task = WFTaskFactory::create_http_task(st->cfg.url, 5, 0,
			[job](WFHttpTask *t) { chunk_response(job, t); });

	task->get_req()->add_header_pair("Range", range);
	task->set_receive_timeout(st->cfg.receive_timeout_sec * 1000);
	task->set_keep_alive(60 * 1000);
	return task;
}

static void finish_worker(ChunkJob *job)
{
	close(job->fd);
	job->st->wg.done();
	delete job;
}

static void advance_after_chunk(ChunkJob *job, SubTask *cur, long long written)
{
	EngineState *st = job->st;

	st->chunks_done.fetch_add(1);
	st->bytes_done.fetch_add(written);

	long long next = claim_chunk(st);
	if (next < 0)
	{
		finish_worker(job);
		return;
	}

	job->idx = next;
	job->attempt = 0;
	series_of(cur)->push_back(make_chunk_http(job));
}

static void schedule_retry(ChunkJob *job, SubTask *cur, long long wait_ms)
{
	WFTimerTask *timer = WFTaskFactory::create_timer_task(
			(unsigned int)(wait_ms / 1000),
			(unsigned int)((wait_ms % 1000) * 1000000),
			[job](WFTimerTask *task) {
		if (job->st->stop.load())		/* interrupted while waiting */
		{
			finish_worker(job);
			return;
		}
		series_of(task)->push_back(make_chunk_http(job));
	});
	series_of(cur)->push_back(timer);
}

static void chunk_response(ChunkJob *job, WFHttpTask *task)
{
	EngineState *st = job->st;
	int http_status = 0;

	if (st->stop.load())
	{
		finish_worker(job);
		return;
	}

	if (task->get_state() == WFT_STATE_SUCCESS)
		http_status = atoi(task->get_resp()->get_status_code());

	ChunkOutcome oc = classify_outcome(http_status);

	switch (oc)
	{
	case ChunkOutcome::SUCCESS:
	{
		long long expected_off = chunk_offset(st->plan, job->idx);
		long long expected_len = chunk_length(st->plan, job->idx);
		std::string cr;
		long long s, e, total;

		if (!resp_header(task, "Content-Range", &cr) ||
			!parse_content_range(cr, &s, &e, &total) ||
			s != expected_off ||
			(total >= 0 && total != st->plan.file_size))
		{
			fail_all(st, "chunk " + std::to_string(job->idx) +
					   ": Content-Range does not match expectations");
			finish_worker(job);
			return;
		}

		std::string body;
		resp_body(task, &body);

		if ((long long)body.size() != e - s + 1 ||
			(long long)body.size() > expected_len)
		{
			/* protocol violation: treat like a transient failure */
			st->retries.fetch_add(1);
			if (attempts_exhausted(job->attempt))
			{
				fail_all(st, "chunk " + std::to_string(job->idx) +
						   ": body length mismatch persists");
				finish_worker(job);
				return;
			}
			schedule_retry(job, task, backoff_ms(job->attempt));
			job->attempt++;
			return;
		}

		void *buf = malloc(body.size());
		if (!buf)
		{
			fail_all(st, "out of memory");
			finish_worker(job);
			return;
		}
		memcpy(buf, body.data(), body.size());

		long long written_len = (long long)body.size();
		WFFileIOTask *pw = WFTaskFactory::create_pwrite_task(
					job->fd, buf, body.size(), (off_t)expected_off,
					[job, buf, written_len](WFFileIOTask *task) {
			free(buf);
			long ret = task->get_retval();
			if (task->get_state() != WFT_STATE_SUCCESS || ret != written_len)
			{
				fail_all(job->st, "write error on chunk " +
								  std::to_string(job->idx));
				finish_worker(job);
				return;
			}
			mark_done(job->st, job->idx);
			advance_after_chunk(job, task, written_len);
		});
		series_of(task)->push_back(pw);
		return;
	}

	case ChunkOutcome::COMPLETE_416:
		/* offset == size: the chunk (and file) is already complete */
		mark_done(st, job->idx);
		advance_after_chunk(job, task, 0);
		return;

	case ChunkOutcome::RATE_LIMITED:
	{
		std::string retry_after;
		resp_header(task, "Retry-After", &retry_after);
		st->retries.fetch_add(1);
		if (attempts_exhausted(job->attempt))
		{
			fail_all(st, "chunk " + std::to_string(job->idx) +
					   ": rate-limited too many times");
			finish_worker(job);
			return;
		}
		schedule_retry(job, task,
					   rate_limited_wait_ms(job->attempt, retry_after));
		job->attempt++;
		return;
	}

	case ChunkOutcome::TRANSIENT:
		st->retries.fetch_add(1);
		if (attempts_exhausted(job->attempt))
		{
			fail_all(st, "chunk " + std::to_string(job->idx) +
					   ": failed after " + std::to_string(job->attempt) +
					   " attempts");
			finish_worker(job);
			return;
		}
		schedule_retry(job, task, backoff_ms(job->attempt));
		job->attempt++;
		return;

	case ChunkOutcome::FATAL:
		fail_all(st, "fatal HTTP status " + std::to_string(http_status));
		finish_worker(job);
		return;

	case ChunkOutcome::MISMATCH:
	default:
		fail_all(st, "unexpected HTTP status " + std::to_string(http_status) +
				   " for chunk " + std::to_string(job->idx));
		finish_worker(job);
		return;
	}
}

static void worker_start(EngineState *st)
{
	int fd = open(st->part_path.c_str(), O_RDWR);
	if (fd < 0)
	{
		fail_all(st, "cannot open part file for writing");
		st->wg.done();
		return;
	}

	long long idx = claim_chunk(st);
	if (idx < 0)
	{
		close(fd);
		st->wg.done();
		return;
	}

	ChunkJob *job = new ChunkJob();
	job->st = st;
	job->idx = idx;
	job->attempt = 0;
	job->fd = fd;

	make_chunk_http(job)->start();
}

/* ---------------- progress ticker ---------------- */

struct TickerCtx
{
	EngineState *st;
	std::atomic<bool> done{false};
	long long last_bytes = 0;
	std::chrono::steady_clock::time_point last_tp =
										std::chrono::steady_clock::now();
};

static void arm_ticker(const std::shared_ptr<TickerCtx>& ctx)
{
	WFTimerTask *timer = WFTaskFactory::create_timer_task(1, 0,
			[ctx](WFTimerTask *) {
		if (ctx->done.load())
			return;

		if (!ctx->st->cfg.quiet)
		{
			auto now = std::chrono::steady_clock::now();
			double dt = std::chrono::duration<double>(now -
												ctx->last_tp).count();
			long long b = ctx->st->bytes_done.load();
			double speed = (dt > 0) ? (b - ctx->last_bytes) / dt : 0;
			ctx->last_bytes = b;
			ctx->last_tp = now;

			ProgressSample s;
			s.total_bytes = ctx->st->plan.file_size;
			s.done_bytes = b;
			s.percent = (s.total_bytes > 0) ?
						100.0 * b / s.total_bytes : -1;
			s.bytes_per_sec = speed;
			s.eta_sec = (speed > 0 && s.total_bytes > b) ?
						(long long)((s.total_bytes - b) / speed) : 0;
			s.chunks_done = ctx->st->chunks_done.load();
			s.chunks_total = ctx->st->plan.chunk_count;
			s.retries = ctx->st->retries.load();
			fprintf(stderr, "%s\n", format_progress_line(s).c_str());
		}

		arm_ticker(ctx);
	});
	timer->start();
}

/* ---------------- orchestration ---------------- */

static bool write_single_stream(const std::string& part_path,
								const std::string& body)
{
	int fd = open(part_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return false;

	ssize_t ret = write(fd, body.data(), body.size());
	close(fd);
	return ret == (ssize_t)body.size();
}

EngineOutcome run_download(const ClientConfig& cfg,
						   const std::string& part_path,
						   const std::string& meta_path)
{
	EngineOutcome out;

	/* ---- probe ---- */
	HeadFetch hf = fetch_head(cfg);

	if (hf.got_response && (hf.status == 403 || hf.status == 404))
	{
		out.error = "fatal HTTP status " + std::to_string(hf.status);
		return out;
	}

	HeadResult hr;
	hr.got_response = hf.got_response;
	hr.status = hf.status;
	hr.content_length = hf.content_length;
	hr.accept_ranges = hf.accept_ranges;
	hr.last_modified = hf.last_modified;

	bool head_usable = hf.got_response && hf.status == 200 &&
					   hf.content_length >= 0;
	ProbeFetch pf;
	ProbeResult pr;

	if (!(head_usable && hf.accept_ranges))
	{
		pf = fetch_probe(cfg);
		pr.performed = true;
		pr.got_response = pf.got_response;
		pr.status = pf.status;
		pr.content_length = pf.content_length;
	}

	if (pf.got_response && (pf.status == 403 || pf.status == 404))
	{
		out.error = "fatal HTTP status " + std::to_string(pf.status);
		return out;
	}

	ProbeDecision dec = decide_mode(hr, pr, cfg.single_stream_limit);

	switch (dec.mode)
	{
	case DownloadMode::FAILED:
		out.error = "cannot determine server Range capability";
		return out;

	case DownloadMode::REFUSE_OVERSIZE:
		out.error = "server lacks Range support and the file exceeds "
					"--single-stream-limit";
		return out;

	case DownloadMode::SINGLE_STREAM:
		if (!pf.got_response)
		{
			out.error = "server probe failed";
			return out;
		}
		if (!write_single_stream(part_path, pf.body))
		{
			out.error = "cannot write part file";
			return out;
		}
		out.success = true;
		out.chunk_mode = false;
		out.total_bytes = (long long)pf.body.size();
		out.last_modified = hr.last_modified;
		return out;

	case DownloadMode::CHUNK:
		break;
	}

	if (dec.known_size < 0)
	{
		out.error = "server did not report the file size";
		return out;
	}

	if (dec.known_size == 0)
	{
		if (!write_single_stream(part_path, ""))
		{
			out.error = "cannot write part file";
			return out;
		}
		out.success = true;
		out.total_bytes = 0;
		out.last_modified = dec.last_modified;
		return out;
	}

	/* ---- plan + resume ---- */
	ChunkPlan plan = make_plan(dec.known_size, cfg.chunk_size, cfg.jobs);
	bool resumed = false;
	DownloadMeta meta;

	if (!meta_path.empty() && load_meta(meta_path, &meta))
	{
		if (meta.url == cfg.url &&
			meta.size == dec.known_size &&
			meta.chunk_size == cfg.chunk_size &&
			meta.bitmap.count() == plan.chunk_count &&
			(dec.last_modified.empty() ||
			 meta.last_modified == dec.last_modified))
		{
			resumed = true;
			fprintf(stderr, "resuming: %lld/%lld chunks already done\n",
					meta.bitmap.done(), plan.chunk_count);
		}
		else
		{
			fprintf(stderr, "warning: saved state does not match "
							"(file changed or different settings); "
							"restarting\n");
			remove(meta_path.c_str());
			remove(part_path.c_str());
			meta = DownloadMeta();
		}
	}

	if (!resumed)
	{
		meta = DownloadMeta();
		meta.url = cfg.url;
		meta.size = dec.known_size;
		meta.last_modified = dec.last_modified;
		meta.chunk_size = cfg.chunk_size;
		meta.bitmap.reset(plan.chunk_count);
	}

	/* preallocate the part file (sparse) */
	{
		int fd = open(part_path.c_str(), O_RDWR | O_CREAT, 0644);
		if (fd < 0)
		{
			out.error = "cannot create part file: " + part_path;
			return out;
		}
		if (ftruncate(fd, (off_t)dec.known_size) < 0)
		{
			close(fd);
			out.error = "cannot extend part file";
			return out;
		}
		close(fd);
	}

	if (meta.bitmap.done() == plan.chunk_count)
	{
		/* everything already downloaded in a previous run */
		out.success = true;
		out.resumed = true;
		out.total_bytes = dec.known_size;
		out.last_modified = dec.last_modified;
		return out;
	}

	/* ---- workers ---- */
	auto st = std::make_unique<EngineState>(plan.workers);
	st->cfg = cfg;
	st->part_path = part_path;
	st->meta_path = meta_path;
	st->plan = plan;
	st->meta = meta;

	/* resumed bytes for progress */
	for (long long i = 0; i < plan.chunk_count; i++)
	{
		if (meta.bitmap.test(i))
			st->bytes_done.fetch_add(chunk_length(plan, i));
	}
	st->chunks_done.store(meta.bitmap.done());

	g_state = st.get();
	signal(SIGINT, sigint_handler);

	auto ticker = std::make_shared<TickerCtx>();
	ticker->st = st.get();
	arm_ticker(ticker);

	for (int i = 0; i < plan.workers; i++)
		worker_start(st.get());

	st->wg.wait();
	ticker->done.store(true);

	g_state = NULL;
	signal(SIGINT, SIG_DFL);

	/* final meta persist (best effort; already saved per mark) */
	if (!meta_path.empty())
	{
		std::lock_guard<std::mutex> lock(st->meta_mu);
		save_meta_atomic(meta_path, st->meta);
	}

	out.retry_count = st->retries.load();

	if (st->sigint_received.load())
	{
		out.interrupted = true;
		return out;
	}

	if (!st->error.empty())
	{
		out.error = st->error;
		return out;
	}

	out.success = true;
	out.resumed = resumed;
	out.total_bytes = dec.known_size;
	out.last_modified = dec.last_modified;
	return out;
}

} // namespace ferry
