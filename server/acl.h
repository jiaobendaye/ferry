#ifndef WF_ACL_H
#define WF_ACL_H

#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "client_ip.h"

namespace ferry
{

/*
 * One rule set. Networks are stored normalized in in6_addr form; IPv4
 * networks are v4-mapped with their prefix length shifted by +96.
 */
struct AclRules
{
	std::vector<std::pair<IpAddr, int>> blacklist;
	std::vector<std::pair<IpAddr, int>> whitelist;
};

/*
 * Parse ACL text. Line format: "blacklist <ip-or-cidr>" or
 * "whitelist <ip-or-cidr>"; '#' comments; blank lines skipped.
 * Returns false if any non-comment line is invalid (rules left untouched).
 */
bool parse_acl_text(const std::string& text, AclRules *rules);

/*
 * Blacklist-priority semantics:
 *   blacklist hit           -> deny (even if also whitelisted)
 *   non-empty whitelist,
 *   no whitelist hit        -> deny
 *   otherwise               -> allow
 */
bool acl_allowed(const AclRules& rules, const IpAddr& ip);

/*
 * Thread-safe holder for the active rule set with hot reload.
 * Readers always observe either the complete old or the complete new set.
 */
class Acl
{
public:
	/*
	 * Startup load. Throws std::runtime_error when the file is missing or
	 * contains invalid lines (fail-closed). An empty file is valid and
	 * means "allow all".
	 */
	explicit Acl(const std::string& path);

	/*
	 * Check the file's mtime; if it changed, re-parse and swap atomically.
	 * A parse failure keeps the previous rules and returns false.
	 * Returns true only when a new rule set was actually swapped in.
	 */
	bool reload_if_changed();

	bool allowed(const IpAddr& ip) const;

	size_t blacklist_size() const;
	size_t whitelist_size() const;

private:
	std::string path_;
	time_t mtime_ = 0;
	mutable std::mutex mutex_;
	std::shared_ptr<const AclRules> rules_;
};

} // namespace ferry

#endif // WF_ACL_H
