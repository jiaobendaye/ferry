#include <cstdio>
#include <string>
#include "progress.h"

namespace ferry
{

std::string format_bytes(long long n)
{
	static const char *units[] = {"B", "KiB", "MiB", "GiB"};
	double v = (double)n;
	int unit = 0;

	while (v >= 1024.0 && unit < 3)
	{
		v /= 1024.0;
		unit++;
	}

	char buf[64];
	snprintf(buf, sizeof(buf), "%.1f%s", v, units[unit]);
	return buf;
}

std::string format_progress_line(const ProgressSample& s)
{
	std::string line;

	if (s.percent >= 0)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%.1f%%", s.percent);
		line += buf;
		line += "  ";
	}

	line += format_bytes(s.done_bytes);
	line += "/";
	line += (s.total_bytes > 0) ? format_bytes(s.total_bytes)
								: std::string("??");

	line += "  ";
	line += format_bytes((long long)s.bytes_per_sec);
	line += "/s";

	if (s.percent >= 0)
		line += "  ETA " + std::to_string(s.eta_sec) + "s";

	if (s.chunks_total > 0)
		line += "  chunks " + std::to_string(s.chunks_done) + "/" +
				std::to_string(s.chunks_total);

	line += "  retries " + std::to_string(s.retries);
	return line;
}

std::string format_summary(long long done_bytes, long long elapsed_ms,
						   double avg_bytes_per_sec, const std::string& sha256)
{
	long long seconds = (elapsed_ms + 500) / 1000;
	std::string line = "done " + format_bytes(done_bytes) +
					   " in " + std::to_string(seconds) +
					   "s (avg " + format_bytes((long long)avg_bytes_per_sec) +
					   "/s)";

	if (!sha256.empty())
		line += " sha256 " + sha256;
	return line;
}

} // namespace ferry
