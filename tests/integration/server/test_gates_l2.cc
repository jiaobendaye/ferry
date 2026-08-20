/*
 * L2 admission-gate tests: a real WFHttpServer with tiny configured
 * limits, driven by an in-process hammer. Small limits turn big signals
 * into short tests.
 *
 * Forged-IP note: with the default trust_hops=1 and no real proxy in
 * front, the client-supplied X-Forwarded-For rightmost entry is the
 * resolved identity — that is exactly how we simulate many distinct
 * clients from one test process.
 */
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFFacilities.h"
#include "workflow/WFHttpServer.h"
#include "workflow/WFTaskFactory.h"

#include "admission/gates.h"
#include "config/config.h"
#include "http/handler.h"
#include "observability/stats.h"

namespace
{

/* ---------------- server under test ---------------- */

class GateServer
{
public:
	GateServer(const ferry::ServerConfig& cfg)
	{
		this->stats = std::make_shared<ferry::Stats>();
		this->gates = ferry::build_gate_chains(cfg);
		this->handler = std::make_shared<ferry::Handler>(
						cfg, nullptr, this->gates.pre, this->gates.post,
						this->stats);
		auto h = this->handler;
		this->server = new WFHttpServer([h](WFHttpTask *task) {
			h->process(task);
		});

		if (this->server->start(0) < 0)		/* ephemeral port */
		{
			perror("gate server start");
			abort();
		}

		struct sockaddr_storage ss;
		socklen_t len = sizeof(ss);
		if (this->server->get_listen_addr((struct sockaddr *)&ss, &len) < 0 ||
			ss.ss_family != AF_INET)
			abort();
		this->port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
	}

	~GateServer()
	{
		this->server->stop();
		delete this->server;
	}

	unsigned short port;
	std::shared_ptr<ferry::Handler> handler;
	std::shared_ptr<ferry::Stats> stats;
	ferry::GateSetup gates;

private:
	WFHttpServer *server;
};

/* ---------------- hammer client ---------------- */

struct OneResponse
{
	bool transport_ok = false;
	std::string status;
	size_t body_size = 0;
	std::string retry_after;
	long long elapsed_ms = 0;
};

using Headers = std::map<std::string, std::string>;
using HeadersFor = std::function<Headers(int)>;

static OneResponse do_request(unsigned short port, const std::string& path,
							  const std::string& method,
							  const Headers& headers)
{
	OneResponse out;
	std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
	WFFacilities::WaitGroup wg(1);
	auto t0 = std::chrono::steady_clock::now();

	WFHttpTask *task = WFTaskFactory::create_http_task(url, 0, 0,
				[&out, &wg](WFHttpTask *task) {
		if (task->get_state() != WFT_STATE_SUCCESS)
		{
			wg.done();
			return;
		}
		out.transport_ok = true;
		protocol::HttpResponse *resp = task->get_resp();
		out.status = resp->get_status_code();

		protocol::HttpHeaderCursor cursor(resp);
		std::string name, value;
		while (cursor.next(name, value))
		{
			if (strcasecmp(name.c_str(), "Retry-After") == 0)
				out.retry_after = value;
		}

		const void *body;
		size_t size;
		if (resp->get_parsed_body(&body, &size))
			out.body_size = size;
		wg.done();
	});

	task->get_req()->set_method(method.c_str());
	for (const auto& h : headers)
		task->get_req()->add_header_pair(h.first.c_str(), h.second.c_str());
	task->start();
	wg.wait();
	out.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - t0).count();
	return out;
}

struct Tally
{
	std::mutex mu;
	std::map<std::string, int> statuses;
	long long bytes = 0;
	int transport_errors = 0;
	long long elapsed_ms = 0;
	std::string retry_after_seen;			/* sample from 429/503 replies */

	int count(const std::string& status)
	{
		auto it = this->statuses.find(status);
		return it == this->statuses.end() ? 0 : it->second;
	}

	int total()
	{
		int n = 0;
		for (const auto& kv : this->statuses)
			n += kv.second;
		return n + this->transport_errors;
	}
};

static void record(Tally *t, const OneResponse& r)
{
	std::lock_guard<std::mutex> lock(t->mu);
	if (!r.transport_ok)
	{
		t->transport_errors++;
		return;
	}
	t->statuses[r.status]++;
	t->bytes += (long long)r.body_size;
	if (!r.retry_after.empty())
		t->retry_after_seen = r.retry_after;
}

/*
 * Closed-loop hammer: `concurrency` worker threads issue sequential
 * requests until the deadline, so the sustained request rate is limited
 * only by the server's responses.
 */
static void hammer_for(Tally *out, unsigned short port,
					   const std::string& path, int concurrency,
					   long long duration_ms, const Headers& headers)
{
	Tally& tally = *out;
	auto deadline = std::chrono::steady_clock::now() +
					std::chrono::milliseconds(duration_ms);
	auto t0 = std::chrono::steady_clock::now();

	std::vector<std::thread> workers;
	for (int i = 0; i < concurrency; i++)
	{
		workers.emplace_back([&]() {
			while (std::chrono::steady_clock::now() < deadline)
				record(&tally, do_request(port, path, "GET", headers));
		});
	}
	for (auto& w : workers)
		w.join();

	tally.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - t0).count();
}

/*
 * Thundering herd: fire `total` requests simultaneously (all tasks in
 * flight at once), wait for every response. headers_for(i) gives each
 * request its own headers (e.g. a unique forged identity).
 */
static void hammer_burst(Tally *out, unsigned short port,
						 const std::string& path, int total,
						 const HeadersFor& headers_for)
{
	Tally& tally = *out;
	auto t0 = std::chrono::steady_clock::now();
	WFFacilities::WaitGroup wg(total);
	std::mutex mu;

	for (int i = 0; i < total; i++)
	{
		std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
		Headers headers = headers_for(i);

		WFHttpTask *task = WFTaskFactory::create_http_task(url, 0, 0,
					[&tally, &mu, &wg](WFHttpTask *task) {
			OneResponse r;
			if (task->get_state() == WFT_STATE_SUCCESS)
			{
				r.transport_ok = true;
				protocol::HttpResponse *resp = task->get_resp();
				r.status = resp->get_status_code();

				protocol::HttpHeaderCursor cursor(resp);
				std::string name, value;
				while (cursor.next(name, value))
				{
					if (strcasecmp(name.c_str(), "Retry-After") == 0)
						r.retry_after = value;
				}
			}
			{
				std::lock_guard<std::mutex> lock(mu);
				record(&tally, r);
			}
			wg.done();
		});

		task->get_req()->set_method("GET");
		for (const auto& h : headers)
			task->get_req()->add_header_pair(h.first.c_str(),
											 h.second.c_str());
		task->start();
	}

	wg.wait();
	tally.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - t0).count();
}

static HeadersFor unique_xff(const std::string& prefix)
{
	return [prefix](int i) {
		return Headers{{"X-Forwarded-For",
						prefix + std::to_string(i / 250) + "." +
						std::to_string(i % 250)}};
	};
}

/* ---------------- fixture ---------------- */

class GatesFixture : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static int counter = 0;
		this->root = "/tmp/ferry_gates_" + std::to_string(getpid()) + "_" +
					 std::to_string(counter++);
		ASSERT_EQ(mkdir(this->root.c_str(), 0755), 0);
		write_file(this->root + "/small.txt", 1000);
		write_file(this->root + "/big.bin", 1024 * 1024);
		write_file(this->root + "/herd.bin", 300);

		this->cfg.port = 0;
		this->cfg.root = this->root;
		this->cfg.cap_bytes = 1024 * 1024;
		this->cfg.size_threshold_bytes = 1024 * 1024;
		this->cfg.max_wait_sec = 1;
		this->cfg.trust_hops = 1;
	}

	void TearDown() override
	{
		(void)system(("rm -rf " + this->root).c_str());
	}

	static void write_file(const std::string& path, size_t size)
	{
		std::ofstream out(path, std::ios::binary);
		for (size_t i = 0; i < size; i++)
			out.put((char)(i % 251));
	}

	std::string root;
	ferry::ServerConfig cfg;
};

/* ---------------- 6.2 thundering herd ---------------- */

TEST_F(GatesFixture, ThunderingHerdAdmitsExactlyTheCap)
{
	/* Every admitted response is held ~2 s in the bandwidth shaper:
	   rate 100 B/s, capacity 100 B, file 300 B -> deficit 200 B.
	   Each request forges its own IP, so the 10 admitted requests hold
	   10 independent buckets (each waits ~2 s) instead of serializing
	   on one. The other 190 collide with a full concurrency gate. */
	this->cfg.max_inflight = 10;
	this->cfg.rate_bytes_per_sec = 100;
	this->cfg.max_wait_sec = 30;
	GateServer s(this->cfg);

	Tally t;
	hammer_burst(&t, s.port, "/herd.bin", 200, unique_xff("10.6."));

	EXPECT_EQ(t.count("200"), 10);			/* exactly the cap admitted */
	EXPECT_EQ(t.count("503"), 190);
	EXPECT_EQ(t.transport_errors, 0);
	EXPECT_EQ(t.retry_after_seen, "1");		/* 503 carries Retry-After: 1 */

	auto snap = s.stats->snapshot();
	EXPECT_EQ(snap.inflight, 0);			/* drained after completion */
	EXPECT_EQ(snap.inflight_peak, 10);
	EXPECT_EQ(snap.gate_rejects[ferry::Stats::GATE_INFLIGHT], 190);
	EXPECT_EQ(s.gates.inflight_limiter->current(), 0);	/* no leaked slots */
}

/* ---------------- 6.3 QPS saturation ---------------- */

TEST_F(GatesFixture, QpsSaturationBoundsAdmittedRate)
{
	this->cfg.qps_total = 200;
	/* max_wait 0 turns the soft shaper into a hard cap: with a closed
	   loop the deficit would otherwise be absorbed as delay and never
	   surface as 429 (the shaping regime is covered by the bandwidth
	   tests). */
	this->cfg.max_wait_sec = 0;
	GateServer s(this->cfg);

	Tally t;
	hammer_for(&t, s.port, "/small.txt", 50, 3000, {});

	long long admitted = t.count("200");
	long long rejected = t.count("429");
	double seconds = t.elapsed_ms / 1000.0;

	ASSERT_GT(rejected, 0);				/* the hammer outruns 200 rps */
	EXPECT_LE(admitted, 200 * seconds + 200 + 50);	/* cap + burst + slack */
	EXPECT_GT(admitted, 100);				/* gate is not a wall */
	EXPECT_EQ(t.transport_errors, 0);

	auto snap = s.stats->snapshot();
	EXPECT_EQ(snap.gate_rejects[ferry::Stats::GATE_QPS_TOTAL], rejected);
	EXPECT_EQ(snap.requests,
			  snap.status_2xx + snap.status_404 +
			  snap.status_4xx_other + snap.status_5xx);
	EXPECT_EQ(snap.inflight, 0);
}

/* ---------------- 6.4 per-IP fairness ---------------- */

TEST_F(GatesFixture, PerIpQuotaDoesNotStarveOtherIPs)
{
	this->cfg.qps_per_ip = 20;
	/* hard cap (see QpsSaturation note): soft shaping would absorb the
	   closed-loop flood as delay instead of 429 */
	this->cfg.max_wait_sec = 0;
	GateServer s(this->cfg);

	Headers xff_a = {{"X-Forwarded-For", "10.9.9.1"}};
	Headers xff_b = {{"X-Forwarded-For", "10.9.9.2"}};

	/* client B stays well under its quota: ~5 req/s for 2 s */
	Tally b_tally;
	std::thread b_worker([&]() {
		auto deadline = std::chrono::steady_clock::now() +
						std::chrono::seconds(2);
		while (std::chrono::steady_clock::now() < deadline)
		{
			record(&b_tally, do_request(s.port, "/small.txt", "GET", xff_b));
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}
	});

	/* client A floods from its own identity */
	Tally a_tally;
	hammer_for(&a_tally, s.port, "/small.txt", 20, 2000, xff_a);
	b_worker.join();

	long long admitted_a = a_tally.count("200");
	EXPECT_GT(a_tally.count("429"), 0);		/* A hit its quota */
	EXPECT_LE(admitted_a, 20 * 2 + 20 + 20);	/* quota + burst + slack */

	/* B sent under its quota and lost nothing to A's flood */
	EXPECT_GE(b_tally.total(), 8);		/* ~5 req/s x 2 s, jitter-tolerant */
	EXPECT_EQ(b_tally.count("200"), b_tally.total());
	EXPECT_EQ(b_tally.count("429"), 0);
	EXPECT_EQ(b_tally.transport_errors, 0);

	EXPECT_GT(s.stats->snapshot().gate_rejects[ferry::Stats::GATE_QPS_PER_IP],
			  0);
}

/* ---------------- 6.5 aggregate bandwidth ---------------- */

TEST_F(GatesFixture, AggregateBandwidthShapesAcrossIPs)
{
	/* total 100 KB/s (capacity 100 KB); three distinct IPs each pull
	   64 KB = 192 KB > capacity, so later requests must wait:
	   ~0 s, ~0.28 s, ~0.92 s. All within max_wait -> all served. */
	this->cfg.rate_total_bps = 100000;
	this->cfg.rate_bytes_per_sec = 100000;	/* per-IP budget is ample */
	this->cfg.max_wait_sec = 30;
	GateServer s(this->cfg);

	std::vector<std::thread> clients;
	std::vector<OneResponse> rs(3);

	for (int i = 0; i < 3; i++)
	{
		clients.emplace_back([&, i]() {
			Headers h = {
				{"X-Forwarded-For", "10.8.8." + std::to_string(i + 1)},
				{"Range", "bytes=0-65535"},
			};
			rs[i] = do_request(s.port, "/big.bin", "GET", h);
		});
	}
	for (auto& c : clients)
		c.join();

	long long max_elapsed = 0;
	for (const auto& r : rs)
	{
		ASSERT_EQ(r.status, "206") << "each request gets ONE response";
		max_elapsed = std::max(max_elapsed, r.elapsed_ms);
	}
	EXPECT_GE(max_elapsed, 200);			/* shaping was visible */

	long long total_bytes = 0;
	for (const auto& r : rs)
		total_bytes += (long long)r.body_size;
	EXPECT_EQ(total_bytes, 3 * 65536);
	EXPECT_EQ(s.stats->snapshot().bytes_served, total_bytes);
}

TEST_F(GatesFixture, AggregateBudgetCanRejectAlone)
{
	/* per-IP budget ample; aggregate budget tiny with a 1 s wait cap:
	   64 KB at 10 KB/s needs ~5.4 s -> a single 429 from rate_total. */
	this->cfg.rate_total_bps = 10000;
	this->cfg.rate_bytes_per_sec = 1048576;
	this->cfg.max_wait_sec = 1;
	GateServer s(this->cfg);

	Headers h = {
		{"X-Forwarded-For", "10.7.7.7"},
		{"Range", "bytes=0-65535"},
	};
	auto r = do_request(s.port, "/big.bin", "GET", h);

	EXPECT_EQ(r.status, "429");
	EXPECT_EQ(r.retry_after, "1");
	EXPECT_EQ(s.stats->snapshot().gate_rejects[ferry::Stats::GATE_RATE_TOTAL],
			  1);
	EXPECT_EQ(s.stats->snapshot().gate_rejects
					[ferry::Stats::GATE_RATE_PER_IP], 0);
}

/* ---------------- 6.6 IP-rotation flood ---------------- */

TEST_F(GatesFixture, RotationFloodBoundedByGlobalGate)
{
	/* 500 distinct forged IPs. With qps_total in front, requests beyond
	   the global cap are rejected before per-IP state is created, so the
	   per-IP bucket map stays far below the request count. */
	this->cfg.qps_total = 50;
	this->cfg.qps_per_ip = 10;
	this->cfg.max_wait_sec = 0;			/* hard cap: reject, don't absorb */
	GateServer s(this->cfg);

	/* Closed-loop workers (a sequential client would self-throttle on
	   the shaping delays): every request forges a fresh identity. */
	Tally tally;
	std::atomic<int> counter{0};
	auto deadline = std::chrono::steady_clock::now() +
					std::chrono::seconds(2);

	std::vector<std::thread> workers;
	for (int w = 0; w < 20; w++)
	{
		workers.emplace_back([&]() {
			while (std::chrono::steady_clock::now() < deadline)
			{
				int i = counter.fetch_add(1);
				Headers h = {
					{"X-Forwarded-For",
					 "10.7." + std::to_string(i / 250) + "." +
					 std::to_string(i % 250)},
				};
				record(&tally,
					   do_request(s.port, "/small.txt", "GET", h));
			}
		});
	}
	for (auto& w : workers)
		w.join();

	int i = counter.load();
	EXPECT_GT(i, 500);					/* drove a real flood */
	EXPECT_GT(tally.count("429"), 0);
	EXPECT_EQ(tally.transport_errors, 0);

	size_t per_ip_entries = s.gates.qps_per_ip_limiter->size();
	/* bounded by global admissions (~qps_total x window), NOT by the
	   number of distinct IPs */
	EXPECT_GT(per_ip_entries, 0u);
	EXPECT_LT(per_ip_entries, (size_t)i / 2);
}

/* ---------------- 4.2 HEAD semantics ---------------- */

TEST_F(GatesFixture, HeadCountsForQpsButNotBandwidth)
{
	this->cfg.qps_per_ip = 2;
	this->cfg.rate_bytes_per_sec = 100;
	this->cfg.max_wait_sec = 1;
	GateServer s(this->cfg);

	/* two HEADs use the 2-unit burst; the third runs a ~500 ms deficit,
	   which is within max_wait -> delayed, still served */
	for (int i = 0; i < 2; i++)
	{
		auto r = do_request(s.port, "/small.txt", "HEAD", {});
		ASSERT_EQ(r.status, "200");
		EXPECT_EQ(r.body_size, 0u);			/* HEAD: headers only */
	}

	auto r4 = do_request(s.port, "/small.txt", "HEAD", {});
	EXPECT_EQ(r4.status, "200");
	EXPECT_GE(r4.elapsed_ms, 150);			/* shaped, not rejected */

	/* HEAD never reaches the post-chain: no per-IP bandwidth bucket */
	EXPECT_EQ(s.gates.bw_per_ip_limiter->size(), 0u);
	/* but it did pass the pre-chain: QPS bucket exists for our IP */
	EXPECT_EQ(s.gates.qps_per_ip_limiter->size(), 1u);
	EXPECT_EQ(s.stats->snapshot().bytes_served, 0);
}

/* ---------------- 4.3 all paths release ---------------- */

TEST_F(GatesFixture, MixedBurstReleasesEverything)
{
	/* cap comfortably above the burst: this test verifies release on
	   every outcome path, not cap enforcement (the herd test does that) */
	this->cfg.max_inflight = 50;
	this->cfg.qps_total = 10000;		/* generous: never the bottleneck */
	GateServer s(this->cfg);

	/* a mix of every outcome class, concurrently */
	std::vector<std::thread> workers;
	for (int i = 0; i < 10; i++)
	{
		workers.emplace_back([&]() {
			do_request(s.port, "/small.txt", "GET", {});			/* 200 */
			do_request(s.port, "/missing.bin", "GET", {});			/* 404 */
			do_request(s.port, "/small.txt", "POST", {});			/* 405 */
			do_request(s.port, "/small.txt", "HEAD", {});			/* 200 */
			do_request(s.port, "/big.bin", "GET",
					   Headers{{"Range", "bytes=99999999-"}});		/* 416 */
		});
	}
	for (auto& w : workers)
		w.join();

	auto snap = s.stats->snapshot();
	EXPECT_EQ(snap.inflight, 0);				/* gauge drains all paths */
	EXPECT_EQ(s.gates.inflight_limiter->current(), 0);	/* no leaked slots */
	EXPECT_EQ(snap.requests, 50);
	EXPECT_EQ(snap.requests,
			  snap.status_2xx + snap.status_404 +
			  snap.status_4xx_other + snap.status_5xx);
	EXPECT_EQ(snap.status_2xx, 20);				/* GET + HEAD */
	EXPECT_EQ(snap.status_404, 10);
	EXPECT_EQ(snap.status_4xx_other, 20);		/* 405 + 416 */
}

} // namespace
