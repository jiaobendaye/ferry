#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include "config.h"

namespace ferry
{

static std::string trim(const std::string& s)
{
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

/* Parse the whole string as an integer; throws on garbage or trailing junk. */
static long long parse_int(const std::string& key, const std::string& value)
{
	const char *s = value.c_str();
	char *end = NULL;

	errno = 0;
	long long v = strtoll(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0')
		throw std::runtime_error("config: invalid value for '" + key +
								 "': \"" + value + "\"");
	return v;
}

static long long parse_range(const std::string& key, const std::string& value,
							 long long min_v, long long max_v)
{
	long long v = parse_int(key, value);
	if (v < min_v || v > max_v)
		throw std::runtime_error("config: value for '" + key +
								 "' out of range [" + std::to_string(min_v) +
								 ", " + std::to_string(max_v) + "]: " +
								 std::to_string(v));
	return v;
}

ServerConfig load_config(const std::string& path)
{
	std::ifstream in(path);
	if (!in)
		throw std::runtime_error("config: cannot open file: " + path);

	ServerConfig cfg;
	std::string line;
	int lineno = 0;

	while (std::getline(in, line))
	{
		lineno++;
		size_t hash = line.find('#');
		if (hash != std::string::npos)
			line.erase(hash);

		line = trim(line);
		if (line.empty())
			continue;

		size_t eq = line.find('=');
		if (eq == std::string::npos)
			throw std::runtime_error("config: line " +
									 std::to_string(lineno) +
									 " is not 'key = value': \"" + line + "\"");

		std::string key = trim(line.substr(0, eq));
		std::string value = trim(line.substr(eq + 1));

		if (key == "port")
			cfg.port = (unsigned short)parse_range(key, value, 1, 65535);
		else if (key == "root")
		{
			if (value.empty())
				throw std::runtime_error("config: 'root' must not be empty");
			cfg.root = value;
		}
		else if (key == "cap_bytes")
			cfg.cap_bytes = parse_range(key, value, 1, 1LL << 40);
		else if (key == "size_threshold_bytes")
			cfg.size_threshold_bytes = parse_range(key, value, 0, 1LL << 40);
		else if (key == "rate_bytes_per_sec")
			cfg.rate_bytes_per_sec = parse_range(key, value, 0, 1LL << 40);
		else if (key == "max_wait_sec")
			cfg.max_wait_sec = (int)parse_range(key, value, 0, 3600);
		else if (key == "trust_hops")
			cfg.trust_hops = (int)parse_range(key, value, 1, 64);
		else if (key == "acl_file")
			cfg.acl_file = value;
		else if (key == "acl_poll_interval_sec")
			cfg.acl_poll_interval_sec = (int)parse_range(key, value, 1, 3600);
		else if (key == "max_connections")
			cfg.max_connections = (int)parse_range(key, value, 1, 10000000);
		else
			fprintf(stderr, "config: %s:%d: unknown key '%s' ignored\n",
					path.c_str(), lineno, key.c_str());
	}

	return cfg;
}

} // namespace ferry
