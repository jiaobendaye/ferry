#ifndef WF_CONFIG_H
#define WF_CONFIG_H

#include <string>

namespace ferry
{

struct ServerConfig
{
	unsigned short port = 8080;
	std::string root = ".";
	long long cap_bytes = 8LL * 1024 * 1024;
	long long size_threshold_bytes = -1;	/* -1 => follows cap_bytes */
	long long rate_bytes_per_sec = 0;		/* 0 => limiting disabled */
	int max_wait_sec = 30;
	int trust_hops = 1;
	std::string acl_file;					/* empty => ACL disabled */
	int acl_poll_interval_sec = 5;
	int max_connections = 2000;

	/* Effective threshold: explicit value if set, otherwise cap. */
	long long threshold() const
	{
		return size_threshold_bytes >= 0 ? size_threshold_bytes : cap_bytes;
	}
};

/*
 * Load configuration from a flat "key = value" file ('#' comments).
 * Throws std::runtime_error naming the offending key when the file is
 * missing or a value is invalid. Unknown keys produce a stderr warning.
 */
ServerConfig load_config(const std::string& path);

} // namespace ferry

#endif // WF_CONFIG_H
