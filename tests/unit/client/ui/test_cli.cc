#include <stdexcept>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "ui/cli.h"

namespace
{

ferry::ClientConfig parse(std::vector<std::string> args)
{
	std::vector<char *> argv;
	argv.push_back((char *)"ferry-client");
	for (std::string& s : args)
		argv.push_back(&s[0]);
	argv.push_back(NULL);
	return ferry::parse_args((int)argv.size() - 1, argv.data());
}

std::string join(const std::vector<std::string>& args)
{
	std::string s;
	for (const std::string& a : args)
		s += (s.empty() ? "" : " ") + a;
	return s;
}

TEST(Cli, DefaultsWhenOnlyUrlGiven)
{
	ferry::ClientConfig cfg = parse({"http://host/dir/big.bin"});
	EXPECT_EQ(cfg.url, "http://host/dir/big.bin");
	EXPECT_TRUE(cfg.output.empty());			/* caller derives basename */
	EXPECT_EQ(cfg.jobs, 4);
	EXPECT_EQ(cfg.chunk_size, 8LL * 1024 * 1024);
	EXPECT_TRUE(cfg.checksum.empty());
	EXPECT_FALSE(cfg.no_verify);
	EXPECT_EQ(cfg.receive_timeout_sec, 60);
	EXPECT_EQ(cfg.single_stream_limit, 256LL * 1024 * 1024);
	EXPECT_FALSE(cfg.quiet);
	EXPECT_FALSE(cfg.show_help);
}

TEST(Cli, ParsesEveryOption)
{
	ferry::ClientConfig cfg = parse({
		"-o", "out.bin",
		"-j", "8",
		"--chunk-size", "16",
		"--checksum", "sha-256=" + std::string(64, 'a'),
		"--no-verify",
		"--receive-timeout", "30",
		"--single-stream-limit", "512",
		"-q",
		"http://host/file"});

	EXPECT_EQ(cfg.url, "http://host/file");
	EXPECT_EQ(cfg.output, "out.bin");
	EXPECT_EQ(cfg.jobs, 8);
	EXPECT_EQ(cfg.chunk_size, 16LL * 1024 * 1024);
	EXPECT_EQ(cfg.checksum, std::string(64, 'a'));
	EXPECT_TRUE(cfg.no_verify);
	EXPECT_EQ(cfg.receive_timeout_sec, 30);
	EXPECT_EQ(cfg.single_stream_limit, 512LL * 1024 * 1024);
	EXPECT_TRUE(cfg.quiet);
	EXPECT_FALSE(cfg.show_help);
}

TEST(Cli, LongOptionForms)
{
	ferry::ClientConfig cfg = parse({
		"--output", "x.bin", "--jobs", "2", "--quiet", "http://host/f"});
	EXPECT_EQ(cfg.output, "x.bin");
	EXPECT_EQ(cfg.jobs, 2);
	EXPECT_TRUE(cfg.quiet);
}

TEST(Cli, BoundaryValuesAccepted)
{
	ferry::ClientConfig cfg = parse({
		"-j", "1", "--chunk-size", "1", "--receive-timeout", "1",
		"--single-stream-limit", "0", "http://host/f"});
	EXPECT_EQ(cfg.jobs, 1);
	EXPECT_EQ(cfg.chunk_size, 1LL * 1024 * 1024);
	EXPECT_EQ(cfg.receive_timeout_sec, 1);
	EXPECT_EQ(cfg.single_stream_limit, 0);
}

TEST(Cli, RejectsInvalidOrOverflowingMibValues)
{
	const std::vector<std::vector<std::string>> bad = {
		{"--chunk-size", "1.5", "http://h/f"},
		{"--chunk-size", "8MiB", "http://h/f"},
		{"--single-stream-limit", "-1", "http://h/f"},
		{"--single-stream-limit", "9223372036854775807", "http://h/f"},
	};

	for (const auto& args : bad)
		EXPECT_THROW(parse(args), std::runtime_error) << "args: " << join(args);
}

TEST(Cli, ChecksumHexIsLowercased)
{
	std::string upper = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
	std::string lower = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	ferry::ClientConfig cfg = parse({"--checksum", "sha-256=" + upper,
									 "http://host/f"});
	EXPECT_EQ(cfg.checksum, lower);
}

TEST(Cli, BasenameOfUrl)
{
	EXPECT_EQ(ferry::basename_of_url("http://host/dir/big.bin"), "big.bin");
	EXPECT_EQ(ferry::basename_of_url("http://host/dir/"), "download");
	EXPECT_EQ(ferry::basename_of_url("http://host/dir/file.bin?v=1&x=2"),
			  "file.bin");
	EXPECT_EQ(ferry::basename_of_url("http://host/dir/file.bin#section"),
			  "file.bin");
	EXPECT_EQ(ferry::basename_of_url(""), "download");
	EXPECT_EQ(ferry::basename_of_url("http://host"), "download");
	EXPECT_EQ(ferry::basename_of_url("http://host/file.tar.gz?a=b#c"),
			  "file.tar.gz");
	EXPECT_EQ(ferry::basename_of_url("http://host/a/b/.."), "download");
}

TEST(Cli, HelpSetsShowHelpWithoutUrl)
{
	ferry::ClientConfig cfg = parse({"--help"});
	EXPECT_TRUE(cfg.show_help);
	EXPECT_TRUE(cfg.url.empty());

	cfg = parse({"-h"});
	EXPECT_TRUE(cfg.show_help);
	EXPECT_TRUE(cfg.url.empty());
}

TEST(Cli, UsageTextMentionsAllOptions)
{
	std::string u = ferry::usage_text();
	EXPECT_NE(u.find("--output"), std::string::npos);
	EXPECT_NE(u.find("--jobs"), std::string::npos);
	EXPECT_NE(u.find("--chunk-size"), std::string::npos);
	EXPECT_NE(u.find("chunk size in MiB"), std::string::npos);
	EXPECT_NE(u.find("--checksum"), std::string::npos);
	EXPECT_NE(u.find("--no-verify"), std::string::npos);
	EXPECT_NE(u.find("--receive-timeout"), std::string::npos);
	EXPECT_NE(u.find("--single-stream-limit"), std::string::npos);
	EXPECT_NE(u.find("size cap in MiB"), std::string::npos);
	EXPECT_NE(u.find("--quiet"), std::string::npos);
	EXPECT_NE(u.find("--help"), std::string::npos);
}

TEST(Cli, InvalidArgumentsThrow)
{
	std::vector<std::vector<std::string>> bad = {
		{},											/* missing URL */
		{"-j", "0", "http://h/f"},
		{"-j", "abc", "http://h/f"},
		{"--jobs", "-1", "http://h/f"},
		{"--frobnicate", "http://h/f"},			/* unknown option */
		{"-x", "http://h/f"},
		{"--checksum", "sha-256=xyz", "http://h/f"},
		{"--checksum", "sha-256=", "http://h/f"},
		{"--checksum", "sha-256=" + std::string(63, 'a'), "http://h/f"},
		{"--checksum", "sha-256=" + std::string(65, 'a'), "http://h/f"},
		{"--checksum", "sha-256=" + std::string(63, 'a') + "g", "http://h/f"},
		{"--checksum", std::string(64, 'a'), "http://h/f"},	/* no prefix */
		{"--checksum", "md5=" + std::string(64, 'a'), "http://h/f"},
		{"--chunk-size", "0", "http://h/f"},
		{"--receive-timeout", "0", "http://h/f"},
		{"http://h/f", "-o"},						/* option value missing */
		{"-o", "", "http://h/f"},					/* empty output */
		{"http://h/f", "http://g/f"},				/* two URLs */
	};

	for (const auto& args : bad)
		EXPECT_THROW(parse(args), std::runtime_error) << "args: " << join(args);
}

} // namespace
