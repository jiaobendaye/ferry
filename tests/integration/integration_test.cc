/*
 * L2 in-process integration tests: a real WFHttpServer on an ephemeral
 * port, driven by workflow HTTP client tasks.
 *
 * Note on ACL hot reload (9.6): the periodic mtime-poll timer lives in
 * main.cc; here reload_if_changed() is driven directly, and the unit
 * suite covers mtime detection. This suite verifies the HTTP effect.
 */
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <gtest/gtest.h>

#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFFacilities.h"
#include "workflow/WFHttpServer.h"
#include "workflow/WFTaskFactory.h"

#include "acl.h"
#include "config.h"
#include "gates.h"
#include "handler.h"
#include "rate_limiter.h"
#include "stats.h"

namespace
{

struct ResponseData
{
	int task_state = -1;
	std::string status;
	std::map<std::string, std::string> headers;	/* lower-cased names */
	std::string body;
	long long elapsed_ms = 0;
};

static std::string lower(std::string s)
{
	for (char& c : s)
		c = tolower((unsigned char)c);
	return s;
}

class TestServer
{
public:
	TestServer(const ferry::ServerConfig& cfg, std::shared_ptr<ferry::Acl> acl)
	{
		this->stats = std::make_shared<ferry::Stats>();
		this->gates = ferry::build_gate_chains(cfg);
		this->handler = std::make_shared<ferry::Handler>(cfg, acl,
						this->gates.pre, this->gates.post, this->stats);
		auto h = this->handler;
		this->server = new WFHttpServer([h](WFHttpTask *task) {
			h->process(task);
		});

		/* ASSERT_* is illegal in constructors; hard-fail instead. */
		if (this->server->start(0) < 0)			/* ephemeral port */
		{
			perror("test server start");
			abort();
		}

		struct sockaddr_storage ss;
		socklen_t len = sizeof(ss);
		if (this->server->get_listen_addr((struct sockaddr *)&ss, &len) < 0 ||
			ss.ss_family != AF_INET)
		{
			perror("test server get_listen_addr");
			abort();
		}
		this->port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
	}

	~TestServer()
	{
		this->server->stop();
		delete this->server;
	}

	ResponseData request(const std::string& path, const std::string& method,
						 const std::map<std::string, std::string>& headers)
	{
		ResponseData data;
		std::string url = "http://127.0.0.1:" + std::to_string(this->port) +
						  path;
		auto t0 = std::chrono::steady_clock::now();
		WFFacilities::WaitGroup wg(1);

		WFHttpTask *task = WFTaskFactory::create_http_task(url, 0, 0,
					[&data, &wg](WFHttpTask *task) {
			data.task_state = task->get_state();
			if (task->get_state() != WFT_STATE_SUCCESS)
			{
				wg.done();
				return;
			}
			protocol::HttpResponse *resp = task->get_resp();
			data.status = resp->get_status_code();

			protocol::HttpHeaderCursor cursor(resp);
			std::string name, value;
			while (cursor.next(name, value))
				data.headers[lower(name)] = value;

			const void *body;
			size_t size;
			if (resp->get_parsed_body(&body, &size))
				data.body.assign((const char *)body, size);

			wg.done();
		});

		task->get_req()->set_method(method.c_str());
		for (const auto& h : headers)
			task->get_req()->add_header_pair(h.first.c_str(), h.second.c_str());

		task->start();
		wg.wait();
		data.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - t0).count();
		return data;
	}

	ResponseData get(const std::string& path,
					 const std::map<std::string, std::string>& headers = {})
	{
		return this->request(path, "GET", headers);
	}

	unsigned short port;
	std::shared_ptr<ferry::Handler> handler;
	std::shared_ptr<ferry::Stats> stats;
	ferry::GateSetup gates;

private:
	WFHttpServer *server;
};

/* ---------------- fixtures on disk ---------------- */

class FileFixture : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static int counter = 0;
		this->root = "/tmp/ferry_int_" + std::to_string(getpid()) + "_" +
					 std::to_string(counter++);
		ASSERT_EQ(mkdir(this->root.c_str(), 0755), 0);
		ASSERT_EQ(mkdir((this->root + "/nested").c_str(), 0755), 0);

		write_file(this->root + "/small.txt", 1000);			/* ≤ cap */
		write_file(this->root + "/big.bin", 3 * 1024 * 1024);	/* > cap */
		write_file(this->root + "/nested/data.txt", 200);
		write_file(this->root + "/empty.bin", 0);

		this->cfg.port = 0;					/* unused: harness picks */
		this->cfg.root = this->root;
		this->cfg.cap_bytes = 1024 * 1024;			/* 1 MiB cap */
		this->cfg.size_threshold_bytes = 1024 * 1024;
		this->cfg.rate_bytes_per_sec = 0;			/* off by default */
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

/* ---------------- status machine ---------------- */

TEST_F(FileFixture, WholeFile200)
{
	TestServer s(this->cfg, nullptr);
	auto r = s.get("/small.txt");

	ASSERT_EQ(r.task_state, WFT_STATE_SUCCESS);
	EXPECT_EQ(r.status, "200");
	EXPECT_EQ(r.body.size(), 1000u);
	EXPECT_EQ(r.headers["content-length"], "1000");
	EXPECT_EQ(r.headers["accept-ranges"], "bytes");
	EXPECT_FALSE(r.headers["last-modified"].empty());
}

TEST_F(FileFixture, NestedPath200)
{
	TestServer s(this->cfg, nullptr);
	auto r = s.get("/nested/data.txt");
	EXPECT_EQ(r.status, "200");
	EXPECT_EQ(r.body.size(), 200u);
}

TEST_F(FileFixture, RangeExact206)
{
	TestServer s(this->cfg, nullptr);
	auto r = s.get("/big.bin", {{"Range", "bytes=0-99"}});

	EXPECT_EQ(r.status, "206");
	EXPECT_EQ(r.headers["content-range"], "bytes 0-99/3145728");
	EXPECT_EQ(r.body.size(), 100u);
	EXPECT_EQ((unsigned char)r.body[0], 0u);
	EXPECT_EQ((unsigned char)r.body[99], 99u);
}

TEST_F(FileFixture, MmapUnalignedRange206)
{
	this->cfg.file_body_mode = ferry::FileBodyMode::MMAP;
	TestServer s(this->cfg, nullptr);
	auto r = s.get("/big.bin", {{"Range", "bytes=123-1000122"}});

	ASSERT_EQ(r.task_state, WFT_STATE_SUCCESS);
	EXPECT_EQ(r.status, "206");
	EXPECT_EQ(r.headers["content-range"], "bytes 123-1000122/3145728");
	ASSERT_EQ(r.body.size(), 1000000u);
	EXPECT_EQ((unsigned char)r.body.front(), (unsigned char)(123 % 251));
	EXPECT_EQ((unsigned char)r.body.back(), (unsigned char)(1000122 % 251));
	auto stats = s.stats->snapshot();
	EXPECT_EQ(stats.mmap_responses, 1);
	EXPECT_EQ(stats.mmap_bytes, 1000000);
	EXPECT_EQ(stats.mmap_active_bytes, 0);
	EXPECT_EQ(stats.mmap_fallbacks, 0);
}

TEST_F(FileFixture, MmapWholeAndEmptyFiles200)
{
	this->cfg.file_body_mode = ferry::FileBodyMode::MMAP;
	TestServer s(this->cfg, nullptr);

	auto whole = s.get("/small.txt");
	EXPECT_EQ(whole.status, "200");
	EXPECT_EQ(whole.body.size(), 1000u);

	auto empty = s.get("/empty.bin");
	EXPECT_EQ(empty.status, "200");
	EXPECT_EQ(empty.headers["content-length"], "0");
	EXPECT_TRUE(empty.body.empty());

	auto stats = s.stats->snapshot();
	EXPECT_EQ(stats.mmap_responses, 1); /* zero-length body needs no mapping */
	EXPECT_EQ(stats.mmap_bytes, 1000);
	EXPECT_EQ(stats.mmap_active_bytes, 0);
}

TEST_F(FileFixture, OpenEndedRangeCapped)
{
	TestServer s(this->cfg, nullptr);
	auto r = s.get("/big.bin", {{"Range", "bytes=0-"}});

	EXPECT_EQ(r.status, "206");
	EXPECT_EQ(r.body.size(), (size_t)this->cfg.cap_bytes);
	EXPECT_EQ(r.headers["content-range"], "bytes 0-1048575/3145728");
}

TEST_F(FileFixture, SuffixRange)
{
	TestServer s(this->cfg, nullptr);
	auto r = s.get("/small.txt", {{"Range", "bytes=-100"}});

	EXPECT_EQ(r.status, "206");
	EXPECT_EQ(r.headers["content-range"], "bytes 900-999/1000");
	EXPECT_EQ(r.body.size(), 100u);
	EXPECT_EQ((unsigned char)r.body[0], (unsigned char)(900 % 251));
}

TEST_F(FileFixture, PastEndIs416)
{
	TestServer s(this->cfg, nullptr);
	auto r = s.get("/small.txt", {{"Range", "bytes=99999-"}});

	EXPECT_EQ(r.status, "416");
	EXPECT_EQ(r.headers["content-range"], "bytes */1000");
	EXPECT_EQ(r.body.find("416"), 0u);	/* error text, no file content */
}

TEST_F(FileFixture, MultiRangeKeepsFirst)
{
	TestServer s(this->cfg, nullptr);
	auto r = s.get("/small.txt", {{"Range", "bytes=0-9,500-509"}});

	EXPECT_EQ(r.status, "206");
	EXPECT_EQ(r.headers["content-range"], "bytes 0-9/1000");
	EXPECT_EQ(r.body.size(), 10u);
}

TEST_F(FileFixture, InvalidUnitFallsBack)
{
	TestServer s(this->cfg, nullptr);
	EXPECT_EQ(s.get("/small.txt", {{"Range", "items=0-9"}}).status, "200");
	EXPECT_EQ(s.get("/big.bin", {{"Range", "items=0-9"}}).status, "413");
}

TEST_F(FileFixture, LargeFileNoRangeIs413)
{
	TestServer s(this->cfg, nullptr);
	auto r = s.get("/big.bin");

	EXPECT_EQ(r.status, "413");
	EXPECT_NE(r.body.find("Range"), std::string::npos);
	EXPECT_NE(r.body.find("3145728"), std::string::npos);
	EXPECT_EQ(r.headers["accept-ranges"], "bytes");
}

TEST_F(FileFixture, MissingIs404)
{
	TestServer s(this->cfg, nullptr);
	auto r = s.get("/nope.bin");
	EXPECT_EQ(r.status, "404");
	EXPECT_EQ(r.headers["accept-ranges"], "bytes");
}

TEST_F(FileFixture, TraversalIs400)
{
	TestServer s(this->cfg, nullptr);
	EXPECT_EQ(s.get("/../etc/passwd").status, "400");
	EXPECT_EQ(s.get("/%2e%2e/%2e%2e/etc/passwd").status, "400");
}

TEST_F(FileFixture, OtherMethodIs405)
{
	TestServer s(this->cfg, nullptr);
	EXPECT_EQ(s.request("/small.txt", "POST", {}).status, "405");
}

TEST_F(FileFixture, IfRangeMatchGives206)
{
	TestServer s(this->cfg, nullptr);

	/* discover Last-Modified first */
	auto head = s.request("/small.txt", "HEAD", {});
	ASSERT_EQ(head.status, "200");
	std::string lm = head.headers["last-modified"];
	ASSERT_FALSE(lm.empty());

	auto r = s.get("/small.txt", {{"Range", "bytes=0-9"}, {"If-Range", lm}});
	EXPECT_EQ(r.status, "206");
	EXPECT_EQ(r.body.size(), 10u);
}

TEST_F(FileFixture, IfRangeStaleFallsBackToWhole)
{
	TestServer s(this->cfg, nullptr);
	auto r = s.get("/small.txt", {
		{"Range", "bytes=0-9"},
		{"If-Range", "Thu, 01 Jan 1970 00:00:00 GMT"}});

	EXPECT_EQ(r.status, "200");		/* non-Range semantics */
	EXPECT_EQ(r.body.size(), 1000u);
}

/* ---------------- HEAD ---------------- */

TEST_F(FileFixture, HeadLargeFileRevealsFullSize)
{
	TestServer s(this->cfg, nullptr);
	auto r = s.request("/big.bin", "HEAD", {});

	EXPECT_EQ(r.status, "200");				/* NOT 413 */
	EXPECT_EQ(r.headers["content-length"], "3145728");
	EXPECT_EQ(r.headers["accept-ranges"], "bytes");
	EXPECT_TRUE(r.body.empty());
}

TEST_F(FileFixture, HeadSmallFile)
{
	TestServer s(this->cfg, nullptr);
	auto r = s.request("/small.txt", "HEAD", {});
	EXPECT_EQ(r.status, "200");
	EXPECT_EQ(r.headers["content-length"], "1000");
	EXPECT_TRUE(r.body.empty());
}

TEST_F(FileFixture, HeadMissingIs404)
{
	TestServer s(this->cfg, nullptr);
	EXPECT_EQ(s.request("/nope", "HEAD", {}).status, "404");
}

/* ---------------- XFF + ACL ---------------- */

class AclFixture : public FileFixture
{
protected:
	void SetUp() override
	{
		FileFixture::SetUp();
		static int counter = 0;
		this->acl_path = "/tmp/ferry_int_acl_" + std::to_string(getpid()) +
						 "_" + std::to_string(counter++) + ".conf";
	}

	void TearDown() override
	{
		remove(this->acl_path.c_str());
		FileFixture::TearDown();
	}

	/* bump seconds must strictly increase across calls in one test */
	void write_acl(const std::string& content, int bump_seconds = 0)
	{
		std::ofstream out(this->acl_path);
		out << content;
		out.close();
		if (bump_seconds > 0)
		{
			struct utimbuf ub;
			ub.actime = time(NULL) + bump_seconds;
			ub.modtime = time(NULL) + bump_seconds;
			utime(this->acl_path.c_str(), &ub);
		}
	}

	std::string acl_path;
};

TEST_F(AclFixture, BlacklistedPeerGets403)
{
	write_acl("blacklist 127.0.0.1\n");
	auto acl = std::make_shared<ferry::Acl>(this->acl_path);
	TestServer s(this->cfg, acl);

	EXPECT_EQ(s.get("/small.txt").status, "403");
}

TEST_F(AclFixture, ForgedLeftmostXffIgnored)
{
	/* rightmost (proxy-appended) is blacklisted; leftmost is whitelisted */
	write_acl("blacklist 127.0.0.1\nwhitelist 10.0.0.1\n");
	auto acl = std::make_shared<ferry::Acl>(this->acl_path);
	TestServer s(this->cfg, acl);

	auto r = s.get("/small.txt", {{"X-Forwarded-For", "10.0.0.1, 127.0.0.1"}});
	EXPECT_EQ(r.status, "403");		/* real IP = 127.0.0.1 (rightmost) */
}

TEST_F(AclFixture, WhitelistGate)
{
	write_acl("whitelist 10.9.9.9\n");
	auto acl = std::make_shared<ferry::Acl>(this->acl_path);
	TestServer s(this->cfg, acl);

	/* peer 127.0.0.1 not whitelisted */
	EXPECT_EQ(s.get("/small.txt").status, "403");

	/* XFF rightmost IS whitelisted */
	auto r = s.get("/small.txt", {{"X-Forwarded-For", "1.2.3.4, 10.9.9.9"}});
	EXPECT_EQ(r.status, "200");
}

TEST_F(AclFixture, NoXffUsesPeerAddress)
{
	write_acl("whitelist 127.0.0.1\n");
	auto acl = std::make_shared<ferry::Acl>(this->acl_path);
	TestServer s(this->cfg, acl);

	EXPECT_EQ(s.get("/small.txt").status, "200");
}

TEST_F(AclFixture, HotReloadChangesBehavior)
{
	write_acl("blacklist 1.1.1.1\n");		/* local peer allowed */
	auto acl = std::make_shared<ferry::Acl>(this->acl_path);
	TestServer s(this->cfg, acl);

	EXPECT_EQ(s.get("/small.txt").status, "200");

	write_acl("blacklist 127.0.0.1\n", 10);
	EXPECT_TRUE(acl->reload_if_changed());
	EXPECT_EQ(s.get("/small.txt").status, "403");

	/* lift again */
	write_acl("# empty\n", 20);
	EXPECT_TRUE(acl->reload_if_changed());
	EXPECT_EQ(s.get("/small.txt").status, "200");
}

TEST_F(AclFixture, BrokenReloadKeepsOldRules)
{
	write_acl("blacklist 127.0.0.1\n");
	auto acl = std::make_shared<ferry::Acl>(this->acl_path);
	TestServer s(this->cfg, acl);
	EXPECT_EQ(s.get("/small.txt").status, "403");

	write_acl("blacklist garbage\n", 10);
	EXPECT_FALSE(acl->reload_if_changed());
	EXPECT_EQ(s.get("/small.txt").status, "403");	/* old rules intact */
}

/* ---------------- rate limiting / soft shaping ---------------- */

TEST_F(FileFixture, RateLimitedShortWaitDelaysThen206)
{
	this->cfg.rate_bytes_per_sec = 100000;	/* 100 KB/s */
	TestServer s(this->cfg, nullptr);

	/* request 1: fresh bucket has 100 KB burst; 64 KB is immediate */
	auto r1 = s.get("/big.bin", {{"Range", "bytes=0-65535"}});
	ASSERT_EQ(r1.status, "206");

	/* request 2: ~36 KB left, needs 64 KB -> ~280 ms wait */
	auto r2 = s.get("/big.bin", {{"Range", "bytes=0-65535"}});
	EXPECT_EQ(r2.status, "206");
	EXPECT_EQ(r2.body.size(), 65536u);
	EXPECT_GE(r2.elapsed_ms, 140);		/* expected*0.5 */
	EXPECT_LE(r2.elapsed_ms, 1120);		/* expected*4 */
}

TEST_F(FileFixture, RateLimitedOverMaxWaitGets429)
{
	this->cfg.rate_bytes_per_sec = 10000;	/* 10 KB/s */
	this->cfg.cap_bytes = 65536;
	TestServer s(this->cfg, nullptr);

	/* 64 KB request against 10 KB bucket: ~5.4 s wait > 1 s max */
	auto r = s.get("/big.bin", {{"Range", "bytes=0-"}});
	EXPECT_EQ(r.status, "429");
	EXPECT_FALSE(r.headers["retry-after"].empty());
	EXPECT_EQ(r.body.find("429"), 0u);	/* error text, no file content */
	EXPECT_LE(r.elapsed_ms, 1000);		/* rejected fast, not delayed */
}

TEST_F(FileFixture, RateDisabledIsUnrestricted)
{
	ASSERT_EQ(this->cfg.rate_bytes_per_sec, 0);
	TestServer s(this->cfg, nullptr);

	auto r = s.get("/big.bin", {{"Range", "bytes=0-"}});
	EXPECT_EQ(r.status, "206");
	EXPECT_LE(r.elapsed_ms, 1000);
}

} // namespace
