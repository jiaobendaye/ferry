#ifndef FERRY_CONFIG_H
#define FERRY_CONFIG_H

#include <string>

namespace ferry
{

enum class FileBodyMode
{
	PREAD,
	MMAP,
};

const char *file_body_mode_name(FileBodyMode mode);

enum class FileCachePolicy
{
	NORMAL,
	NOREUSE,
	DROP_AFTER_READ,
};

const char *file_cache_policy_name(FileCachePolicy policy);

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
	FileBodyMode file_body_mode = FileBodyMode::PREAD;
	FileCachePolicy page_cache_policy = FileCachePolicy::NORMAL;

	/* Admission gates; each 0 => gate disabled. Global gates protect
	   the server (identity-independent caps); per-IP gates divide the
	   budget fairly among clients. */
	long long qps_total = 0;				/* whole-server req/s cap */
	long long qps_per_ip = 0;				/* per-IP req/s quota */
	int max_inflight = 0;					/* whole-server in-flight cap */
	int max_inflight_per_ip = 0;			/* per-IP in-flight quota */
	long long rate_total_bps = 0;			/* whole-server bytes/s cap */
	int stats_interval_sec = 0;				/* periodic stats line */

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

#endif // FERRY_CONFIG_H
