#include <arpa/inet.h>
#include <cstring>
#include <gtest/gtest.h>
#include "client_ip.h"

namespace
{

struct sockaddr_in peer_v4(const char *ip, in_port_t port = 1234)
{
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	inet_pton(AF_INET, ip, &sa.sin_addr);
	return sa;
}

TEST(ParseIp, IPv4AndIPv6)
{
	ferry::IpAddr ip;
	ASSERT_TRUE(ferry::parse_ip("192.168.1.57", &ip));
	EXPECT_TRUE(ip.is_v4_mapped());
	EXPECT_EQ(ip.to_string(), "192.168.1.57");

	ASSERT_TRUE(ferry::parse_ip("2001:db8::1", &ip));
	EXPECT_FALSE(ip.is_v4_mapped());
	EXPECT_EQ(ip.to_string(), "2001:db8::1");

	EXPECT_FALSE(ferry::parse_ip("not-an-ip", &ip));
	EXPECT_FALSE(ferry::parse_ip("1.2.3", &ip));
	EXPECT_FALSE(ferry::parse_ip("", &ip));
}

TEST(ParseIp, IPv6NormalizationEquivalence)
{
	ferry::IpAddr a, b;
	ASSERT_TRUE(ferry::parse_ip("2001:db8::1", &a));
	ASSERT_TRUE(ferry::parse_ip("2001:0db8:0000:0000:0000:0000:0000:0001", &b));
	EXPECT_TRUE(a == b);
	EXPECT_EQ(a.to_string(), b.to_string());
}

TEST(ResolveClientIp, RightmostSelected)
{
	struct sockaddr_in peer = peer_v4("9.9.9.9");
	ferry::IpAddr ip;

	/* forged leftmost, proxy-appended rightmost */
	ASSERT_TRUE(ferry::resolve_client_ip("1.2.3.4, 5.6.7.8", 1,
				(struct sockaddr *)&peer, sizeof(peer), &ip));
	EXPECT_EQ(ip.to_string(), "5.6.7.8");
}

TEST(ResolveClientIp, ForgedLeftmostIgnored)
{
	struct sockaddr_in peer = peer_v4("9.9.9.9");
	ferry::IpAddr ip;

	ASSERT_TRUE(ferry::resolve_client_ip("10.0.0.1, 203.0.113.7", 1,
				(struct sockaddr *)&peer, sizeof(peer), &ip));
	EXPECT_EQ(ip.to_string(), "203.0.113.7");
	EXPECT_NE(ip.to_string(), "10.0.0.1");
}

TEST(ResolveClientIp, TrustHopsTwo)
{
	struct sockaddr_in peer = peer_v4("9.9.9.9");
	ferry::IpAddr ip;

	ASSERT_TRUE(ferry::resolve_client_ip("1.1.1.1, 2.2.2.2, 3.3.3.3", 2,
				(struct sockaddr *)&peer, sizeof(peer), &ip));
	EXPECT_EQ(ip.to_string(), "2.2.2.2");
}

TEST(ResolveClientIp, MissingHeaderFallsBackToPeer)
{
	struct sockaddr_in peer = peer_v4("9.9.9.9");
	ferry::IpAddr ip;

	ASSERT_TRUE(ferry::resolve_client_ip("", 1,
				(struct sockaddr *)&peer, sizeof(peer), &ip));
	EXPECT_EQ(ip.to_string(), "9.9.9.9");
}

TEST(ResolveClientIp, GarbageSelectedFallsBackToPeer)
{
	struct sockaddr_in peer = peer_v4("9.9.9.9");
	ferry::IpAddr ip;

	ASSERT_TRUE(ferry::resolve_client_ip("1.1.1.1, garbage", 1,
				(struct sockaddr *)&peer, sizeof(peer), &ip));
	EXPECT_EQ(ip.to_string(), "9.9.9.9");
}

TEST(ResolveClientIp, HeaderShorterThanHopsFallsBack)
{
	struct sockaddr_in peer = peer_v4("9.9.9.9");
	ferry::IpAddr ip;

	ASSERT_TRUE(ferry::resolve_client_ip("1.1.1.1", 3,
				(struct sockaddr *)&peer, sizeof(peer), &ip));
	EXPECT_EQ(ip.to_string(), "9.9.9.9");
}

TEST(ResolveClientIp, IPv6XffEntry)
{
	struct sockaddr_in peer = peer_v4("9.9.9.9");
	ferry::IpAddr ip;

	ASSERT_TRUE(ferry::resolve_client_ip("spoofed, 2001:db8::7", 1,
				(struct sockaddr *)&peer, sizeof(peer), &ip));
	EXPECT_EQ(ip.to_string(), "2001:db8::7");
}

} // namespace
