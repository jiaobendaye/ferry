#include <gtest/gtest.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include "verify.h"

namespace
{

/*
 * Digest vectors obtained independently with sha256sum:
 *   printf 'abc\n' | sha256sum
 *   python3 -c 'write bytes(i % 251 for i in range(20 MiB))' | sha256sum
 */
const char *EMPTY_DIGEST =
	"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const char *ABC_DIGEST =
	"edeaaff3f1774ad2888673770c6d64097e391bc362d7d6fb34982ddf0efd18cb";
const char *PATTERN_20MIB_DIGEST =
	"99254018a4506cae413a471f8b9d968a1ab1771565f3247b6e1c3f927e9a572f";

std::string tmp_path(const char *name)
{
	return "/tmp/ferry_verify_" + std::to_string(getpid()) + "_" + name;
}

void write_file(const std::string& path, const std::string& contents)
{
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f.write(contents.data(), (std::streamsize)contents.size());
}

/* 20 MiB of byte i % 251: three 8 MiB-default chunks, short last chunk. */
void write_pattern_file(const std::string& path)
{
	const long long SIZE = 20LL * 1024 * 1024;
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	std::vector<char> buf(64 * 1024);
	long long written = 0;

	while (written < SIZE)
	{
		size_t n = (size_t)std::min((long long)buf.size(), SIZE - written);
		for (size_t i = 0; i < n; i++)
			buf[i] = (char)((written + i) % 251);
		f.write(buf.data(), (std::streamsize)n);
		written += n;
	}
}

TEST(Sha256OfFile, EmptyFile)
{
	std::string path = tmp_path("empty");
	write_file(path, "");
	EXPECT_EQ(ferry::sha256_of_file(path), EMPTY_DIGEST);
	remove(path.c_str());
}

TEST(Sha256OfFile, SmallText)
{
	std::string path = tmp_path("abc");
	write_file(path, "abc\n");
	EXPECT_EQ(ferry::sha256_of_file(path), ABC_DIGEST);
	remove(path.c_str());
}

TEST(Sha256OfFile, MultiChunkPattern)
{
	std::string path = tmp_path("pattern20m");
	write_pattern_file(path);
	EXPECT_EQ(ferry::sha256_of_file(path), PATTERN_20MIB_DIGEST);
	remove(path.c_str());
}

TEST(Sha256OfFile, SmallChunkSizeMatchesDefault)
{
	std::string path = tmp_path("chunksizes");
	write_file(path, "abc\n");
	EXPECT_EQ(ferry::sha256_of_file(path, 1), ABC_DIGEST);
	EXPECT_EQ(ferry::sha256_of_file(path, 7), ABC_DIGEST);
	EXPECT_EQ(ferry::sha256_of_file(path, 4), ABC_DIGEST);
	remove(path.c_str());
}

TEST(Sha256OfFile, MissingPathReturnsEmpty)
{
	EXPECT_EQ(ferry::sha256_of_file(tmp_path("does-not-exist")), "");
}

TEST(Sha256OfFile, DirectoryReturnsEmpty)
{
	EXPECT_EQ(ferry::sha256_of_file("/tmp"), "");
}

TEST(ChecksumSpecMatches, LowercaseExact)
{
	EXPECT_TRUE(ferry::checksum_spec_matches(
		std::string("sha-256=") + ABC_DIGEST, ABC_DIGEST));
}

TEST(ChecksumSpecMatches, UppercaseHexMatches)
{
	std::string upper(ABC_DIGEST);
	for (char& c : upper)
	{
		if (c >= 'a' && c <= 'f')
			c = c - 'a' + 'A';
	}

	EXPECT_TRUE(ferry::checksum_spec_matches("sha-256=" + upper, ABC_DIGEST));
}

TEST(ChecksumSpecMatches, PrefixCaseInsensitive)
{
	EXPECT_TRUE(ferry::checksum_spec_matches(
		std::string("SHA-256=") + ABC_DIGEST, ABC_DIGEST));
	EXPECT_TRUE(ferry::checksum_spec_matches(
		std::string("Sha-256=") + ABC_DIGEST, ABC_DIGEST));
}

TEST(ChecksumSpecMatches, OtherSchemeRejected)
{
	EXPECT_FALSE(ferry::checksum_spec_matches(
		std::string("md5=") + ABC_DIGEST, ABC_DIGEST));
	EXPECT_FALSE(ferry::checksum_spec_matches(
		std::string("sha256=") + ABC_DIGEST, ABC_DIGEST));
}

TEST(ChecksumSpecMatches, MalformedRejected)
{
	EXPECT_FALSE(ferry::checksum_spec_matches("", ABC_DIGEST));
	EXPECT_FALSE(ferry::checksum_spec_matches("sha-256=", ABC_DIGEST));
	EXPECT_FALSE(ferry::checksum_spec_matches("sha-256", ABC_DIGEST));
	EXPECT_FALSE(ferry::checksum_spec_matches("garbage", ABC_DIGEST));
	/* right scheme, wrong digest */
	EXPECT_FALSE(ferry::checksum_spec_matches(
		std::string("sha-256=") + EMPTY_DIGEST, ABC_DIGEST));
}

} // namespace
