#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <utime.h>
#include "access/acl.h"

namespace
{

ferry::IpAddr ip(const char *text)
{
	ferry::IpAddr out;
	EXPECT_TRUE(ferry::parse_ip(text, &out));
	return out;
}

TEST(ParseAclText, ValidEntries)
{
	ferry::AclRules rules;
	ASSERT_TRUE(ferry::parse_acl_text(
		"# comment\n"
		"blacklist 192.168.1.0/24\n"
		"blacklist 2001:db8:dead::/48\n"
		"whitelist 10.0.0.5\n"
		"\n", &rules));
	EXPECT_EQ(rules.blacklist.size(), 2u);
	EXPECT_EQ(rules.whitelist.size(), 1u);
}

TEST(ParseAclText, InvalidLinesRejected)
{
	ferry::AclRules rules;
	const char *bad[] = {
		"blacklist not-an-ip",
		"blacklist 1.2.3.4/64",		/* v4 prefix > 32 */
		"graylist 1.2.3.4",			/* unknown kind */
		"blacklist",					/* missing entry */
	};
	for (const char *text : bad)
		EXPECT_FALSE(ferry::parse_acl_text(text, &rules)) << text;
}

TEST(AclAllowed, IPv4Subnet)
{
	ferry::AclRules rules;
	ASSERT_TRUE(ferry::parse_acl_text("blacklist 192.168.1.0/24\n", &rules));

	EXPECT_FALSE(ferry::acl_allowed(rules, ip("192.168.1.57")));
	EXPECT_TRUE(ferry::acl_allowed(rules, ip("192.168.2.1")));
	EXPECT_TRUE(ferry::acl_allowed(rules, ip("10.0.0.1")));
}

TEST(AclAllowed, IPv6Prefix)
{
	ferry::AclRules rules;
	ASSERT_TRUE(ferry::parse_acl_text("blacklist 2001:db8::/32\n", &rules));

	EXPECT_FALSE(ferry::acl_allowed(rules, ip("2001:db8:1::1")));
	EXPECT_TRUE(ferry::acl_allowed(rules, ip("2001:db9::1")));
}

TEST(AclAllowed, BareIpIsHostRoute)
{
	ferry::AclRules rules;
	ASSERT_TRUE(ferry::parse_acl_text("whitelist 10.0.0.5\n", &rules));

	EXPECT_TRUE(ferry::acl_allowed(rules, ip("10.0.0.5")));
	EXPECT_FALSE(ferry::acl_allowed(rules, ip("10.0.0.6")));
}

TEST(AclAllowed, BlacklistBeatsWhitelist)
{
	ferry::AclRules rules;
	ASSERT_TRUE(ferry::parse_acl_text(
		"blacklist 10.0.0.5\n"
		"whitelist 10.0.0.0/8\n", &rules));

	EXPECT_FALSE(ferry::acl_allowed(rules, ip("10.0.0.5")));
	EXPECT_TRUE(ferry::acl_allowed(rules, ip("10.1.2.3")));
}

TEST(AclAllowed, EmptyWhitelistAllowsAll)
{
	ferry::AclRules rules;
	ASSERT_TRUE(ferry::parse_acl_text("blacklist 10.0.0.5\n", &rules));

	EXPECT_TRUE(ferry::acl_allowed(rules, ip("8.8.8.8")));
}

TEST(AclAllowed, IPv6FormsMatchConsistently)
{
	ferry::AclRules rules;
	ASSERT_TRUE(ferry::parse_acl_text(
		"blacklist 2001:0db8:0000:0000:0000:0000:0000:0001/128\n", &rules));
	EXPECT_FALSE(ferry::acl_allowed(rules, ip("2001:db8::1")));
}

class TmpAclFile
{
public:
	TmpAclFile()
	{
		static int counter = 0;
		path_ = "/tmp/ferry_acl_test_" + std::to_string(getpid()) + "_" +
				std::to_string(counter++) + ".conf";
	}
	~TmpAclFile() { remove(path_.c_str()); }

	void write(const std::string& content, time_t mtime_offset = 0)
	{
		std::ofstream out(path_);
		out << content;
		out.close();
		if (mtime_offset)
		{
			struct utimbuf ub;
			ub.actime = time(NULL) + mtime_offset;
			ub.modtime = time(NULL) + mtime_offset;
			utime(path_.c_str(), &ub);
		}
	}

	const std::string& path() const { return path_; }

private:
	std::string path_;
};

TEST(Acl, StartupLoadAndEmptyFile)
{
	TmpAclFile f;
	f.write("# nothing\n");
	ferry::Acl acl(f.path());
	EXPECT_EQ(acl.blacklist_size(), 0u);
	EXPECT_TRUE(acl.allowed(ip("1.2.3.4")));
}

TEST(Acl, MissingFileThrows)
{
	EXPECT_THROW(ferry::Acl acl("/tmp/ferry_acl_no_such_file.conf"),
				 std::runtime_error);
}

TEST(Acl, BrokenFileThrowsAtStartup)
{
	TmpAclFile f;
	f.write("blacklist garbage\n");
	EXPECT_THROW(ferry::Acl acl(f.path()), std::runtime_error);
}

TEST(Acl, HotReloadSwapsRules)
{
	TmpAclFile f;
	f.write("blacklist 1.1.1.1\n");
	ferry::Acl acl(f.path());

	EXPECT_FALSE(acl.allowed(ip("1.1.1.1")));
	EXPECT_TRUE(acl.allowed(ip("2.2.2.2")));
	EXPECT_FALSE(acl.reload_if_changed());	/* unchanged */

	f.write("blacklist 2.2.2.2\n", +10);	/* bump mtime */
	EXPECT_TRUE(acl.reload_if_changed());

	EXPECT_TRUE(acl.allowed(ip("1.1.1.1")));
	EXPECT_FALSE(acl.allowed(ip("2.2.2.2")));
}

TEST(Acl, BrokenReloadKeepsOldRules)
{
	TmpAclFile f;
	f.write("blacklist 1.1.1.1\n");
	ferry::Acl acl(f.path());

	f.write("blacklist garbage\n", +10);
	EXPECT_FALSE(acl.reload_if_changed());

	EXPECT_FALSE(acl.allowed(ip("1.1.1.1")));	/* old rules intact */
}

} // namespace
