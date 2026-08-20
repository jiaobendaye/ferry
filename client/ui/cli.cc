#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#include "cli.h"

namespace ferry
{

/* Parse the whole string as an integer; throws on garbage or trailing junk. */
static long long parse_int(const std::string& opt, const std::string& value)
{
	const char *s = value.c_str();
	char *end = NULL;

	errno = 0;
	long long v = strtoll(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0')
		throw std::runtime_error("ferry-client: invalid value for '" + opt +
								 "': \"" + value + "\"");
	return v;
}

static long long parse_at_least(const std::string& opt, const std::string& value,
								long long min_v)
{
	long long v = parse_int(opt, value);
	if (v < min_v)
		throw std::runtime_error("ferry-client: value for '" + opt +
								 "' must be >= " + std::to_string(min_v) +
								 ", got " + std::to_string(v));
	return v;
}

/* Accepts "sha-256=<64 hex chars>"; returns the digest lowercased. */
static std::string parse_checksum(const std::string& value)
{
	static const std::string prefix = "sha-256=";

	if (value.compare(0, prefix.size(), prefix) != 0)
		throw std::runtime_error("ferry-client: --checksum must be "
								 "'sha-256=<64 hex chars>'");

	std::string hex = value.substr(prefix.size());
	if (hex.size() != 64)
		throw std::runtime_error("ferry-client: --checksum digest must be 64 "
								 "hex characters, got " +
								 std::to_string(hex.size()));

	for (size_t i = 0; i < hex.size(); i++)
	{
		if (!isxdigit((unsigned char)hex[i]))
			throw std::runtime_error("ferry-client: --checksum digest is not "
									 "hexadecimal: \"" + hex + "\"");
		hex[i] = (char)tolower((unsigned char)hex[i]);
	}

	return hex;
}

std::string basename_of_url(const std::string& url)
{
	std::string s = url;

	/* strip fragment and query */
	size_t cut = s.find_first_of("#?");
	if (cut != std::string::npos)
		s.erase(cut);

	/* strip "scheme://host" if a scheme is present */
	size_t scheme = s.find("://");
	if (scheme != std::string::npos)
	{
		size_t path_begin = s.find('/', scheme + 3);
		s = (path_begin == std::string::npos) ? "" : s.substr(path_begin);
	}

	size_t last = s.find_last_of('/');
	std::string base = (last == std::string::npos) ? s : s.substr(last + 1);
	if (base.empty() || base == "." || base == "..")
		return "download";
	return base;
}

std::string usage_text()
{
	return
		"usage: ferry-client [options] <url>\n"
		"  -o, --output PATH            output file (default: basename of the URL)\n"
		"  -j, --jobs N                 parallel workers (default 4)\n"
		"  --chunk-size BYTES           chunk size in bytes (default 8388608 = 8MiB)\n"
		"  --checksum sha-256=HEX       expected sha256 digest of the whole file\n"
		"  --no-verify                  skip the final sha256 pass\n"
		"  --receive-timeout SEC        per-request receive timeout (default 60)\n"
		"  --single-stream-limit BYTES  size cap for non-Range downloads\n"
		"                               (default 268435456 = 256MiB)\n"
		"  -q, --quiet                  suppress progress lines\n"
		"  -h, --help                   show this help\n";
}

ClientConfig parse_args(int argc, char **argv)
{
	ClientConfig cfg;
	std::vector<std::string> positionals;

	for (int i = 1; i < argc; i++)
	{
		std::string arg = argv[i];

		if (arg.size() < 2 || arg[0] != '-')
		{
			positionals.push_back(arg);
			continue;
		}

		std::string name = arg;
		std::string inline_value;
		bool has_inline = false;

		size_t eq = arg.find('=');
		if (arg.compare(0, 2, "--") == 0 && eq != std::string::npos)
		{
			name = arg.substr(0, eq);
			inline_value = arg.substr(eq + 1);
			has_inline = true;
		}

		/* Value for a valued option: inline "--opt=value" or the next arg. */
		auto take = [&](const std::string& opt) -> std::string {
			if (has_inline)
				return inline_value;
			if (i + 1 >= argc)
				throw std::runtime_error("ferry-client: option '" + opt +
										 "' requires a value");
			return argv[++i];
		};

		auto reject_value = [&](const std::string& opt) {
			if (has_inline)
				throw std::runtime_error("ferry-client: option '" + opt +
										 "' does not take a value");
		};

		if (name == "-h" || name == "--help")
		{
			reject_value(name);
			cfg.show_help = true;
		}
		else if (name == "-q" || name == "--quiet")
		{
			reject_value(name);
			cfg.quiet = true;
		}
		else if (name == "--no-verify")
		{
			reject_value(name);
			cfg.no_verify = true;
		}
		else if (name == "-o" || name == "--output")
		{
			cfg.output = take(name);
			if (cfg.output.empty())
				throw std::runtime_error("ferry-client: option '" + name +
										 "' must not be empty");
		}
		else if (name == "-j" || name == "--jobs")
			cfg.jobs = (int)parse_at_least(name, take(name), 1);
		else if (name == "--chunk-size")
			cfg.chunk_size = parse_at_least(name, take(name), 1);
		else if (name == "--checksum")
			cfg.checksum = parse_checksum(take(name));
		else if (name == "--receive-timeout")
			cfg.receive_timeout_sec = (int)parse_at_least(name, take(name), 1);
		else if (name == "--single-stream-limit")
			cfg.single_stream_limit = parse_at_least(name, take(name), 0);
		else
			throw std::runtime_error("ferry-client: unknown option '" +
									 name + "'");
	}

	if (positionals.size() > 1)
		throw std::runtime_error("ferry-client: expected exactly one URL, got " +
								 std::to_string(positionals.size()));
	if (!positionals.empty())
		cfg.url = positionals[0];

	if (cfg.url.empty() && !cfg.show_help)
		throw std::runtime_error("ferry-client: no URL given "
								 "(see -h/--help for usage)");

	return cfg;
}

} // namespace ferry
