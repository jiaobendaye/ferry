#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include "config/config.h"

namespace
{

class TmpConfig
{
public:
	explicit TmpConfig(const std::string& content)
	{
		static int counter = 0;
		path_ = "/tmp/ferry_cfg_test_" + std::to_string(getpid()) + "_" +
				std::to_string(counter++) + ".conf";
		std::ofstream out(path_);
		out << content;
	}

	~TmpConfig() { remove(path_.c_str()); }

	const std::string& path() const { return path_; }

private:
	std::string path_;
};

TEST(Config, ParsesAllKeys)
{
	TmpConfig f(
		"port = 9090\n"
		"root = /srv/files\n"
		"cap_bytes = 1048576\n"
		"size_threshold_bytes = 2097152\n"
		"rate_bytes_per_sec = 512000\n"
		"max_wait_sec = 10\n"
		"trust_hops = 2\n"
		"acl_file = /etc/acl.conf\n"
		"acl_poll_interval_sec = 3\n"
		"max_connections = 500\n"
		"file_body_mode = mmap\n"
		"qps_total = 1000\n"
		"qps_per_ip = 20\n"
		"max_inflight = 500\n"
		"max_inflight_per_ip = 8\n"
		"rate_total_bps = 10485760\n"
		"stats_interval_sec = 30\n");

	ferry::ServerConfig cfg = ferry::load_config(f.path());
	EXPECT_EQ(cfg.port, 9090);
	EXPECT_EQ(cfg.root, "/srv/files");
	EXPECT_EQ(cfg.cap_bytes, 1048576);
	EXPECT_EQ(cfg.size_threshold_bytes, 2097152);
	EXPECT_EQ(cfg.threshold(), 2097152);
	EXPECT_EQ(cfg.rate_bytes_per_sec, 512000);
	EXPECT_EQ(cfg.max_wait_sec, 10);
	EXPECT_EQ(cfg.trust_hops, 2);
	EXPECT_EQ(cfg.acl_file, "/etc/acl.conf");
	EXPECT_EQ(cfg.acl_poll_interval_sec, 3);
	EXPECT_EQ(cfg.max_connections, 500);
	EXPECT_EQ(cfg.file_body_mode, ferry::FileBodyMode::MMAP);
	EXPECT_STREQ(ferry::file_body_mode_name(cfg.file_body_mode), "mmap");
	EXPECT_EQ(cfg.qps_total, 1000);
	EXPECT_EQ(cfg.qps_per_ip, 20);
	EXPECT_EQ(cfg.max_inflight, 500);
	EXPECT_EQ(cfg.max_inflight_per_ip, 8);
	EXPECT_EQ(cfg.rate_total_bps, 10485760);
	EXPECT_EQ(cfg.stats_interval_sec, 30);
}

TEST(Config, DefaultsAndComments)
{
	TmpConfig f(
		"# a comment\n"
		"\n"
		"port = 81   # trailing comment\n"
		"  root=/tmp  \n");

	ferry::ServerConfig cfg = ferry::load_config(f.path());
	EXPECT_EQ(cfg.port, 81);
	EXPECT_EQ(cfg.root, "/tmp");
	EXPECT_EQ(cfg.cap_bytes, 8LL * 1024 * 1024);
	EXPECT_EQ(cfg.size_threshold_bytes, -1);
	EXPECT_EQ(cfg.threshold(), cfg.cap_bytes);	/* follows cap */
	EXPECT_EQ(cfg.rate_bytes_per_sec, 0);		/* limiting disabled */
	EXPECT_EQ(cfg.max_wait_sec, 30);
	EXPECT_EQ(cfg.trust_hops, 1);
	EXPECT_TRUE(cfg.acl_file.empty());
	EXPECT_EQ(cfg.qps_total, 0);				/* gates off by default */
	EXPECT_EQ(cfg.qps_per_ip, 0);
	EXPECT_EQ(cfg.max_inflight, 0);
	EXPECT_EQ(cfg.max_inflight_per_ip, 0);
	EXPECT_EQ(cfg.rate_total_bps, 0);
	EXPECT_EQ(cfg.stats_interval_sec, 0);
	EXPECT_EQ(cfg.file_body_mode, ferry::FileBodyMode::PREAD);
}

TEST(Config, UnknownKeyIsTolerated)
{
	TmpConfig f("port = 82\nno_such_key = 42\n");
	ferry::ServerConfig cfg = ferry::load_config(f.path());
	EXPECT_EQ(cfg.port, 82);
}

TEST(Config, MissingFileThrows)
{
	EXPECT_THROW(ferry::load_config("/tmp/ferry_no_such_file_xyz.conf"),
				 std::runtime_error);
}

TEST(Config, InvalidValuesThrow)
{
	const char *bad[] = {
		"cap_bytes = -1",
		"cap_bytes = abc",
		"cap_bytes = 12x",
		"port = 0",
		"port = 70000",
		"port = -5",
		"max_wait_sec = -1",
		"trust_hops = 0",
		"acl_poll_interval_sec = 0",
		"max_connections = 0",
		"size_threshold_bytes = -3",
		"root = ",
		"this is not key value",
		"qps_total = -1",
		"qps_per_ip = -5",
		"max_inflight = -1",
		"max_inflight_per_ip = -2",
		"rate_total_bps = -100",
		"stats_interval_sec = -30",
		"file_body_mode = sendfile",
		"file_body_mode = MMAP",
	};

	for (const char *content : bad)
	{
		TmpConfig f(content);
		EXPECT_THROW(ferry::load_config(f.path()), std::runtime_error)
			<< "content: " << content;
	}
}

} // namespace
