#include <arpa/inet.h>
#include <cstring>
#include <vector>
#include "client_ip.h"

namespace ferry
{

bool IpAddr::operator==(const IpAddr& other) const
{
	return memcmp(&this->addr, &other.addr, sizeof(struct in6_addr)) == 0;
}

bool IpAddr::is_v4_mapped() const
{
	return IN6_IS_ADDR_V4MAPPED(&this->addr);
}

std::string IpAddr::to_string() const
{
	char buf[INET6_ADDRSTRLEN];

	if (this->is_v4_mapped())
	{
		struct in_addr v4;
		memcpy(&v4, &this->addr.s6_addr[12], 4);
		if (inet_ntop(AF_INET, &v4, buf, sizeof(buf)))
			return buf;
	}
	else if (inet_ntop(AF_INET6, &this->addr, buf, sizeof(buf)))
		return buf;

	return "";
}

static IpAddr make_v4_mapped(const struct in_addr *v4)
{
	IpAddr ip;
	memset(&ip.addr, 0, sizeof(ip.addr));
	ip.addr.s6_addr[10] = 0xff;
	ip.addr.s6_addr[11] = 0xff;
	memcpy(&ip.addr.s6_addr[12], v4, 4);
	return ip;
}

bool parse_ip(const std::string& text, IpAddr *out)
{
	struct in_addr v4;
	struct in6_addr v6;

	if (inet_pton(AF_INET, text.c_str(), &v4) == 1)
	{
		*out = make_v4_mapped(&v4);
		return true;
	}

	if (inet_pton(AF_INET6, text.c_str(), &v6) == 1)
	{
		IpAddr ip;
		ip.addr = v6;
		*out = ip;
		return true;
	}

	return false;
}

static bool from_sockaddr(const struct sockaddr *sa, socklen_t len, IpAddr *out)
{
	if (!sa)
		return false;

	if (sa->sa_family == AF_INET && len >= (socklen_t)sizeof(struct sockaddr_in))
	{
		const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
		*out = make_v4_mapped(&sin->sin_addr);
		return true;
	}

	if (sa->sa_family == AF_INET6 &&
		len >= (socklen_t)sizeof(struct sockaddr_in6))
	{
		const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
		IpAddr ip;
		ip.addr = sin6->sin6_addr;
		*out = ip;
		return true;
	}

	return false;
}

static std::string trim(const std::string& s)
{
	size_t b = s.find_first_not_of(" \t");
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(" \t");
	return s.substr(b, e - b + 1);
}

bool resolve_client_ip(const std::string& xff, int trust_hops,
					   const struct sockaddr *peer, socklen_t peer_len,
					   IpAddr *out)
{
	if (!xff.empty() && trust_hops > 0)
	{
		std::vector<std::string> entries;
		size_t start = 0;

		while (start <= xff.size())
		{
			size_t comma = xff.find(',', start);
			size_t end = (comma == std::string::npos ? xff.size() : comma);
			entries.push_back(trim(xff.substr(start, end - start)));
			if (comma == std::string::npos)
				break;
			start = comma + 1;
		}

		if ((int)entries.size() >= trust_hops)
		{
			const std::string& selected = entries[entries.size() - trust_hops];
			if (!selected.empty() && parse_ip(selected, out))
				return true;
		}
		/* selected entry missing or unparseable -> fall through */
	}

	return from_sockaddr(peer, peer_len, out);
}

} // namespace ferry
