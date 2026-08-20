#include <gtest/gtest.h>
#include "http/path.h"

namespace
{

bool maps(const char *uri, std::string *out)
{
	return ferry::uri_to_safe_path("/srv/files", uri, out);
}

TEST(PathSafety, SimplePath)
{
	std::string out;
	ASSERT_TRUE(maps("/a/b.txt", &out));
	EXPECT_EQ(out, "/srv/files/a/b.txt");
}

TEST(PathSafety, RootPath)
{
	std::string out;
	ASSERT_TRUE(maps("/", &out));
	EXPECT_EQ(out, "/srv/files");
}

TEST(PathSafety, QueryStripped)
{
	std::string out;
	ASSERT_TRUE(maps("/a.txt?x=1&y=2", &out));
	EXPECT_EQ(out, "/srv/files/a.txt");
}

TEST(PathSafety, TraversalRejected)
{
	std::string out;
	EXPECT_FALSE(maps("/../etc/passwd", &out));
	EXPECT_FALSE(maps("/a/../../etc/passwd", &out));
	EXPECT_FALSE(maps("..", &out));
	EXPECT_FALSE(maps("/a/b/../../../x", &out));
}

TEST(PathSafety, InnerDotDotResolvesInside)
{
	std::string out;
	ASSERT_TRUE(maps("/a/b/../c.txt", &out));
	EXPECT_EQ(out, "/srv/files/a/c.txt");
}

TEST(PathSafety, PercentEncodedTraversalRejected)
{
	std::string out;
	EXPECT_FALSE(maps("/%2e%2e/%2e%2e/etc/passwd", &out));
	EXPECT_FALSE(maps("/%2E%2E/x", &out));
}

TEST(PathSafety, PercentDecodingWorks)
{
	std::string out;
	ASSERT_TRUE(maps("/dir%20one/my%20file.txt", &out));
	EXPECT_EQ(out, "/srv/files/dir one/my file.txt");
}

TEST(PathSafety, InvalidPercentRejected)
{
	std::string out;
	EXPECT_FALSE(maps("/bad%zz", &out));
	EXPECT_FALSE(maps("/trailing%2", &out));
}

TEST(PathSafety, NulByteRejected)
{
	std::string out;
	EXPECT_FALSE(maps("/a%00b.txt", &out));
}

TEST(PathSafety, DotSegmentsSkipped)
{
	std::string out;
	ASSERT_TRUE(maps("/./a/./b.txt", &out));
	EXPECT_EQ(out, "/srv/files/a/b.txt");
}

} // namespace
