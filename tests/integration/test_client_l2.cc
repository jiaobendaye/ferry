/*
 * L2 closed-loop integration tests: real ferry::Handler serving on an
 * ephemeral port + real ferry client engine downloading from it.
 */
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <gtest/gtest.h>

#include "workflow/HttpMessage.h"
#include "workflow/WFHttpServer.h"

#include "acl.h"
#include "config.h"
#include "gates.h"
#include "handler.h"
#include "rate_limiter.h"
#include "stats.h"

#include "bitmap.h"
#include "cli.h"
#include "engine.h"
#include "verify.h"

namespace
{

class LoopServer
{
public:
	LoopServer(const ferry::ServerConfig& scfg, std::shared_ptr<ferry::Acl> acl)
	{
		this->stats = std::make_shared<ferry::Stats>();
		this->gates = ferry::build_gate_chains(scfg);
		this->handler = std::make_shared<ferry::Handler>(scfg, acl,
						this->gates.pre, this->gates.post, this->stats);
		auto h = this->handler;
		this->server = new WFHttpServer([h](WFHttpTask *task) {
			h->process(task);
		});

		if (this->server->start(0) < 0)
		{
			perror("loop server start");
			abort();
		}

		struct sockaddr_storage ss;
		socklen_t len = sizeof(ss);
		if (this->server->get_listen_addr((struct sockaddr *)&ss, &len) < 0)
			abort();
		this->port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
	}

	~LoopServer()
	{
		this->server->stop();
		delete this->server;
	}

	std::string url(const std::string& path)
	{
		return "http://127.0.0.1:" + std::to_string(this->port) + path;
	}

	unsigned short port;
	std::shared_ptr<ferry::Handler> handler;
	std::shared_ptr<ferry::Stats> stats;
	ferry::GateSetup gates;

private:
	WFHttpServer *server;
};

/* Server-side fixture files + config */
class ClientLoop : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static int counter = 0;
		this->tag = "/tmp/ferry_l2_" + std::to_string(getpid()) + "_" +
					std::to_string(counter++);
		this->root = this->tag + "_files";
		this->workdir = this->tag + "_out";
		ASSERT_EQ(mkdir(this->root.c_str(), 0755), 0);
		ASSERT_EQ(mkdir(this->workdir.c_str(), 0755), 0);

		write_pattern(this->root + "/small.bin", 1000);
		write_pattern(this->root + "/big.bin", 3 * 1024 * 1024);	/* 3 MiB */

		this->scfg.port = 0;
		this->scfg.root = this->root;
		this->scfg.cap_bytes = 1024 * 1024;			/* 1 MiB server cap */
		this->scfg.size_threshold_bytes = 1024 * 1024;
		this->scfg.rate_bytes_per_sec = 0;
		this->scfg.max_wait_sec = 1;
		this->scfg.trust_hops = 1;
	}

	void TearDown() override
	{
		(void)system(("rm -rf " + this->root + " " + this->workdir + " " +
					  this->tag + "*").c_str());
	}

	static void write_pattern(const std::string& path, size_t size)
	{
		std::ofstream out(path, std::ios::binary);
		for (size_t i = 0; i < size; i++)
			out.put((char)(i % 251));
	}

	ferry::ClientConfig client_cfg(const std::string& url,
								   const std::string& name)
	{
		ferry::ClientConfig cfg;
		cfg.url = url;
		cfg.output = this->workdir + "/" + name;
		cfg.jobs = 4;
		cfg.chunk_size = 512 * 1024;		/* 512 KiB client chunks */
		cfg.receive_timeout_sec = 60;
		cfg.single_stream_limit = 256LL * 1024 * 1024;
		cfg.quiet = true;
		return cfg;
	}

	/* run_download + the finalization main.cc performs on success */
	ferry::EngineOutcome run(const ferry::ClientConfig& cfg)
	{
		auto out = ferry::run_download(cfg, cfg.output + ".part",
									   cfg.output + ".ferry.json");
		if (out.success)
		{
			rename((cfg.output + ".part").c_str(), cfg.output.c_str());
			remove((cfg.output + ".ferry.json").c_str());
		}
		return out;
	}

	void expect_same(const std::string& src, const std::string& dst)
	{
		std::string a = ferry::sha256_of_file(src);
		std::string b = ferry::sha256_of_file(dst);
		ASSERT_FALSE(a.empty());
		EXPECT_EQ(a, b);
	}

	std::string tag, root, workdir;
	ferry::ServerConfig scfg;
};

TEST_F(ClientLoop, FullDownloadMatchesSource)
{
	LoopServer s(this->scfg, nullptr);
	auto cfg = client_cfg(s.url("/big.bin"), "big.bin");

	auto out = run(cfg);
	ASSERT_TRUE(out.success) << out.error;
	EXPECT_TRUE(out.chunk_mode);
	EXPECT_EQ(out.total_bytes, 3 * 1024 * 1024);
	expect_same(this->root + "/big.bin", cfg.output);
	/* meta cleaned up after success */
	EXPECT_NE(access((cfg.output + ".ferry.json").c_str(), F_OK), 0);
}

TEST_F(ClientLoop, SmallFileSingleChunk)
{
	LoopServer s(this->scfg, nullptr);
	auto cfg = client_cfg(s.url("/small.bin"), "small.bin");
	cfg.jobs = 1;

	auto out = run(cfg);
	ASSERT_TRUE(out.success) << out.error;
	EXPECT_EQ(out.total_bytes, 1000);
	expect_same(this->root + "/small.bin", cfg.output);
}

TEST_F(ClientLoop, SingleWorker)
{
	LoopServer s(this->scfg, nullptr);
	auto cfg = client_cfg(s.url("/big.bin"), "big.bin");
	cfg.jobs = 1;

	auto out = run(cfg);
	ASSERT_TRUE(out.success) << out.error;
	expect_same(this->root + "/big.bin", cfg.output);
}

TEST_F(ClientLoop, ChunkSizeExactMultiple)
{
	/* 3 MiB file, 1 MiB client chunks -> exactly 3 chunks */
	LoopServer s(this->scfg, nullptr);
	auto cfg = client_cfg(s.url("/big.bin"), "big.bin");
	cfg.chunk_size = 1024 * 1024;

	auto out = run(cfg);
	ASSERT_TRUE(out.success) << out.error;
	expect_same(this->root + "/big.bin", cfg.output);
}

TEST_F(ClientLoop, SlowServerStillSucceeds)
{
	/* server shapes at 100 KB/s; 200 KB file -> a few seconds, no errors */
	write_pattern(this->root + "/shaped.bin", 200 * 1024);
	this->scfg.rate_bytes_per_sec = 100000;
	this->scfg.max_wait_sec = 30;		/* was passed to the limiter directly */
	LoopServer s(this->scfg, nullptr);

	auto cfg = client_cfg(s.url("/shaped.bin"), "shaped.bin");
	cfg.chunk_size = 64 * 1024;

	auto t0 = std::chrono::steady_clock::now();
	auto out = run(cfg);
	long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - t0).count();

	ASSERT_TRUE(out.success) << out.error;
	EXPECT_GE(ms, 800);					/* shaping must be visible */
	expect_same(this->root + "/shaped.bin", cfg.output);
}

TEST_F(ClientLoop, RateLimited429RetriesObserved)
{
	/* 10 KB/s with max_wait 1s; 512 KB chunks are always far over budget,
	   so every attempt gets 429 and the client waits per Retry-After /
	   backoff. Observe several retries, then interrupt. */
	this->scfg.rate_bytes_per_sec = 10000;
	LoopServer s(this->scfg, nullptr);

	auto cfg = client_cfg(s.url("/big.bin"), "big.bin");
	cfg.jobs = 1;
	cfg.chunk_size = 512 * 1024;

	ferry::EngineOutcome out;
	std::thread th([&]() { out = run(cfg); });
	std::this_thread::sleep_for(std::chrono::milliseconds(6000));
	raise(SIGINT);
	th.join();

	ASSERT_TRUE(out.interrupted);
	EXPECT_GE(out.retry_count, 2);		/* 429s were retried with waits */

	/* zero progress is possible at this rate: bitmap must be empty */
	ferry::DownloadMeta meta;
	ASSERT_TRUE(ferry::load_meta(cfg.output + ".ferry.json", &meta));
	EXPECT_EQ(meta.bitmap.done(), 0);
	EXPECT_EQ(access((cfg.output + ".part").c_str(), F_OK), 0);
}

TEST_F(ClientLoop, InterruptThenResume)
{
	/* 1 MB/s shaping: 1 MB burst then ~128 ms per 128 KB chunk, so SIGINT
	   at 1.2 s lands mid-download; resume against the SAME server (same
	   URL) must detect and reuse the saved state. */
	this->scfg.rate_bytes_per_sec = 1048576;
	this->scfg.max_wait_sec = 30;		/* was passed to the limiter directly */
	LoopServer s(this->scfg, nullptr);

	auto cfg = client_cfg(s.url("/big.bin"), "big.bin");
	cfg.chunk_size = 128 * 1024;
	cfg.jobs = 1;

	ferry::EngineOutcome first;
	std::thread th([&]() { first = run(cfg); });
	std::this_thread::sleep_for(std::chrono::milliseconds(1200));
	raise(SIGINT);
	th.join();

	ASSERT_TRUE(first.interrupted);
	EXPECT_EQ(access((cfg.output + ".part").c_str(), F_OK), 0);
	EXPECT_EQ(access((cfg.output + ".ferry.json").c_str(), F_OK), 0);

	ferry::DownloadMeta meta;
	ASSERT_TRUE(ferry::load_meta(cfg.output + ".ferry.json", &meta));
	long long done_before = meta.bitmap.done();
	EXPECT_GT(done_before, 0);

	/* resume: same server, same URL, same settings */
	auto second = run(cfg);
	ASSERT_TRUE(second.success) << second.error;
	EXPECT_TRUE(second.resumed);
	expect_same(this->root + "/big.bin", cfg.output);
}

TEST_F(ClientLoop, ChangedFileRestartsCleanly)
{
	this->scfg.rate_bytes_per_sec = 1048576;
	this->scfg.max_wait_sec = 30;		/* was passed to the limiter directly */
	LoopServer s(this->scfg, nullptr);

	auto cfg = client_cfg(s.url("/big.bin"), "big.bin");
	cfg.chunk_size = 128 * 1024;
	cfg.jobs = 1;

	ferry::EngineOutcome first;
	std::thread th([&]() { first = run(cfg); });
	std::this_thread::sleep_for(std::chrono::milliseconds(1200));
	raise(SIGINT);
	th.join();
	ASSERT_TRUE(first.interrupted);

	/* bump the source file's mtime -> Last-Modified changes */
	struct utimbuf ub;
	ub.actime = time(NULL) + 60;
	ub.modtime = time(NULL) + 60;
	ASSERT_EQ(utime((this->root + "/big.bin").c_str(), &ub), 0);

	/* same server/URL: state must be discarded on Last-Modified mismatch */
	auto second = run(cfg);
	ASSERT_TRUE(second.success) << second.error;
	EXPECT_FALSE(second.resumed);		/* state discarded, fresh start */
	expect_same(this->root + "/big.bin", cfg.output);
}

TEST_F(ClientLoop, ForbiddenIsFatal)
{
	std::string acl_path = this->tag + "_acl.conf";
	{
		std::ofstream out(acl_path);
		out << "blacklist 127.0.0.1\n";
	}
	auto acl = std::make_shared<ferry::Acl>(acl_path);
	LoopServer s(this->scfg, acl);

	auto cfg = client_cfg(s.url("/small.bin"), "small.bin");
	auto out = run(cfg);
	EXPECT_FALSE(out.success);
	EXPECT_NE(out.error.find("403"), std::string::npos);
	remove(acl_path.c_str());
}

TEST_F(ClientLoop, MissingFileIsFatal)
{
	LoopServer s(this->scfg, nullptr);
	auto cfg = client_cfg(s.url("/nope.bin"), "nope.bin");
	auto out = run(cfg);
	EXPECT_FALSE(out.success);
	EXPECT_NE(out.error.find("404"), std::string::npos);
}

/* ------- single-stream fallback against a Range-less server ------- */

class RangelessServer
{
public:
	explicit RangelessServer(const std::string& root)
	{
		this->server = new WFHttpServer([root](WFHttpTask *task) {
			protocol::HttpRequest *req = task->get_req();
			protocol::HttpResponse *resp = task->get_resp();
			const char *uri = req->get_request_uri();
			std::string path = root + std::string(uri);

			FILE *fp = fopen(path.c_str(), "rb");
			if (!fp)
			{
				resp->set_status_code("404");
				return;
			}
			fseek(fp, 0, SEEK_END);
			long size = ftell(fp);
			fseek(fp, 0, SEEK_SET);

			char cl[32];
			snprintf(cl, sizeof(cl), "%ld", size);
			resp->add_header_pair("Content-Length", cl);
			/* deliberately NO Accept-Ranges and Range is ignored */

			if (strcmp(req->get_method(), "HEAD") != 0)
			{
				std::string body(size, '\0');
				size_t got = fread(&body[0], 1, size, fp);
				resp->append_output_body(body.data(), got);
			}
			fclose(fp);
		});

		if (this->server->start(0) < 0)
			abort();

		struct sockaddr_storage ss;
		socklen_t len = sizeof(ss);
		this->server->get_listen_addr((struct sockaddr *)&ss, &len);
		this->port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
	}

	~RangelessServer()
	{
		this->server->stop();
		delete this->server;
	}

	std::string url(const std::string& path)
	{
		return "http://127.0.0.1:" + std::to_string(this->port) + path;
	}

	unsigned short port;

private:
	WFHttpServer *server;
};

TEST_F(ClientLoop, SingleStreamFallback)
{
	RangelessServer s(this->root);
	auto cfg = client_cfg(s.url("/small.bin"), "small.bin");

	auto out = run(cfg);
	ASSERT_TRUE(out.success) << out.error;
	EXPECT_FALSE(out.chunk_mode);
	EXPECT_EQ(out.total_bytes, 1000);
	expect_same(this->root + "/small.bin", cfg.output);
}

TEST_F(ClientLoop, SingleStreamRefusesOversize)
{
	RangelessServer s(this->root);
	auto cfg = client_cfg(s.url("/big.bin"), "big.bin");
	cfg.single_stream_limit = 1024;			/* 3 MiB > 1 KiB limit */

	auto out = run(cfg);
	EXPECT_FALSE(out.success);
	EXPECT_NE(out.error.find("single-stream-limit"), std::string::npos);
}

} // namespace
