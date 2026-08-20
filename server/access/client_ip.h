#ifndef WF_CLIENT_IP_H
#define WF_CLIENT_IP_H

#include <netinet/in.h>
#include <string>

namespace ferry
{

/*
 * Normalized IP address: always stored as struct in6_addr; IPv4 addresses
 * are kept in v4-mapped form (::ffff:a.b.c.d) so that matching and keying
 * have a single representation.
 */
struct IpAddr
{
	struct in6_addr addr;

	bool operator==(const IpAddr& other) const;
	bool operator!=(const IpAddr& other) const { return !(*this == other); }

	bool is_v4_mapped() const;
	std::string to_string() const;	/* dotted quad for v4, hex for v6 */
};

/* Parse an IPv4 or IPv6 literal. Returns false if unparseable. */
bool parse_ip(const std::string& text, IpAddr *out);

/*
 * Resolve the real client IP.
 *
 * xff: raw X-Forwarded-For header value ("" when absent). The entry at
 * position `trust_hops` counted from the RIGHT is selected (rightmost =
 * the one appended by the nearest trusted proxy; spoof-resistant).
 * peer: the connection's socket peer address (fallback).
 *
 * Returns false only when neither the header nor the peer address yields
 * a usable IP.
 */
bool resolve_client_ip(const std::string& xff, int trust_hops,
					   const struct sockaddr *peer, socklen_t peer_len,
					   IpAddr *out);

} // namespace ferry

#endif // WF_CLIENT_IP_H
