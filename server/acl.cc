#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include "acl.h"

namespace ferry
{

/* Compare the first `prefix` bits of two v6-form addresses. */
static bool prefix_match(const struct in6_addr& a, const struct in6_addr& b,
						 int prefix)
{
	int full = prefix / 8;
	int rem = prefix % 8;

	if (full > 0 && memcmp(&a, &b, full) != 0)
		return false;

	if (rem > 0)
	{
		unsigned char mask = (unsigned char)(0xFF00 >> rem);
		if ((a.s6_addr[full] & mask) != (b.s6_addr[full] & mask))
			return false;
	}

	return true;
}

static bool match_any(const std::vector<std::pair<IpAddr, int>>& rules,
					  const IpAddr& ip)
{
	for (const auto& rule : rules)
	{
		if (prefix_match(ip.addr, rule.first.addr, rule.second))
			return true;
	}
	return false;
}

bool acl_allowed(const AclRules& rules, const IpAddr& ip)
{
	if (match_any(rules.blacklist, ip))
		return false;

	if (!rules.whitelist.empty() && !match_any(rules.whitelist, ip))
		return false;

	return true;
}

static std::string trim(const std::string& s)
{
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

/*
 * Parse "ip-or-cidr" into a normalized network + prefix.
 * IPv4 networks are stored v4-mapped with prefix shifted by +96.
 */
static bool parse_cidr(const std::string& text, IpAddr *net, int *prefix)
{
	std::string addr_part = text;
	int explicit_prefix = -1;

	size_t slash = text.find('/');
	if (slash != std::string::npos)
	{
		addr_part = text.substr(0, slash);
		std::string pfx = text.substr(slash + 1);

		char *end = NULL;
		long v = strtol(pfx.c_str(), &end, 10);
		if (end == pfx.c_str() || *end != '\0' || v < 0 || v > 128)
			return false;
		explicit_prefix = (int)v;
	}

	IpAddr ip;
	if (!parse_ip(addr_part, &ip))
		return false;

	int default_prefix = ip.is_v4_mapped() ? 32 : 128;
	int p = (explicit_prefix >= 0 ? explicit_prefix : default_prefix);

	if (ip.is_v4_mapped())
	{
		if (p > 32)
			return false;
		p += 96;						/* v4-mapped: skip ::ffff prefix */
	}

	*net = ip;
	*prefix = p;
	return true;
}

bool parse_acl_text(const std::string& text, AclRules *rules)
{
	AclRules parsed;
	std::istringstream in(text);
	std::string line;

	while (std::getline(in, line))
	{
		size_t hash = line.find('#');
		if (hash != std::string::npos)
			line.erase(hash);

		line = trim(line);
		if (line.empty())
			continue;

		size_t sp = line.find_first_of(" \t");
		if (sp == std::string::npos)
			return false;

		std::string kind = trim(line.substr(0, sp));
		std::string entry = trim(line.substr(sp + 1));

		IpAddr net;
		int prefix;
		if (!parse_cidr(entry, &net, &prefix))
			return false;

		if (kind == "blacklist")
			parsed.blacklist.push_back({net, prefix});
		else if (kind == "whitelist")
			parsed.whitelist.push_back({net, prefix});
		else
			return false;
	}

	*rules = parsed;
	return true;
}

static bool read_file(const std::string& path, std::string *content)
{
	std::ifstream in(path);
	if (!in)
		return false;

	std::ostringstream ss;
	ss << in.rdbuf();
	*content = ss.str();
	return true;
}

static time_t file_mtime(const std::string& path)
{
	struct stat st;
	if (stat(path.c_str(), &st) < 0)
		return -1;
	return st.st_mtime;
}

Acl::Acl(const std::string& path) : path_(path)
{
	std::string content;
	if (!read_file(path, &content))
		throw std::runtime_error("acl: cannot open file: " + path);

	auto rules = std::make_shared<AclRules>();
	if (!parse_acl_text(content, rules.get()))
		throw std::runtime_error("acl: invalid rules in file: " + path);

	this->rules_ = rules;
	this->mtime_ = file_mtime(path);
}

bool Acl::reload_if_changed()
{
	time_t mtime = file_mtime(this->path_);
	if (mtime < 0 || mtime == this->mtime_)
		return false;					/* nothing to do */

	std::string content;
	if (!read_file(this->path_, &content))
	{
		fprintf(stderr, "acl: reload failed (unreadable), keeping old rules: %s\n",
				this->path_.c_str());
		this->mtime_ = mtime;			/* don't hammer a broken file */
		return false;
	}

	auto rules = std::make_shared<AclRules>();
	if (!parse_acl_text(content, rules.get()))
	{
		fprintf(stderr, "acl: reload failed (parse error), keeping old rules: %s\n",
				this->path_.c_str());
		this->mtime_ = mtime;
		return false;
	}

	std::lock_guard<std::mutex> lock(this->mutex_);
	this->rules_ = rules;
	this->mtime_ = mtime;
	return true;
}

bool Acl::allowed(const IpAddr& ip) const
{
	std::shared_ptr<const AclRules> rules;
	{
		std::lock_guard<std::mutex> lock(this->mutex_);
		rules = this->rules_;
	}
	return acl_allowed(*rules, ip);
}

size_t Acl::blacklist_size() const
{
	std::lock_guard<std::mutex> lock(this->mutex_);
	return this->rules_->blacklist.size();
}

size_t Acl::whitelist_size() const
{
	std::lock_guard<std::mutex> lock(this->mutex_);
	return this->rules_->whitelist.size();
}

} // namespace ferry
