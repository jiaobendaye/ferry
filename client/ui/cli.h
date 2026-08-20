#ifndef FERRY_CLIENT_CLI_H
#define FERRY_CLIENT_CLI_H

#include <string>

namespace ferry
{

struct ClientConfig
{
	std::string url;
	std::string output;						/* empty => caller uses basename_of_url(url) */
	int jobs = 4;
	long long chunk_size = 8LL * 1024 * 1024;
	std::string checksum;					/* "" or the validated, lowercased hex part
											 * of "sha-256=<hex>" */
	bool no_verify = false;
	int receive_timeout_sec = 60;
	long long single_stream_limit = 256LL * 1024 * 1024;
	bool quiet = false;
	bool show_help = false;
};

/*
 * Parse the command line. Exactly one positional URL is required unless
 * -h/--help is given (which sets show_help and needs no URL). Throws
 * std::runtime_error describing the problem on invalid arguments.
 */
ClientConfig parse_args(int argc, char **argv);

/*
 * Last path segment of the URL, stripped of query and fragment; url-decoding
 * is not performed. Returns "download" when nothing sensible remains.
 */
std::string basename_of_url(const std::string& url);

std::string usage_text();

} // namespace ferry

#endif // FERRY_CLIENT_CLI_H
