#ifndef FERRY_CLIENT_PROGRESS_H
#define FERRY_CLIENT_PROGRESS_H

#include <string>

namespace ferry
{

struct ProgressSample
{
	double percent = 0;			/* 0..100; negative => total unknown, the
								 * formatters omit percent and ETA */
	long long done_bytes = 0;
	long long total_bytes = 0;	/* 0 => unknown */
	double bytes_per_sec = 0;
	long long eta_sec = 0;
	long long chunks_done = 0;
	long long chunks_total = 0;
	long long retries = 0;
};

/*
 * One-line progress report, e.g.
 * "45.2%  378.0MiB/834.0MiB  52.3MiB/s  ETA 9s  chunks 48/105  retries 3".
 * When the size is unknown (percent < 0) percent and ETA are omitted and
 * the total is shown as "??"; the chunks part is omitted when chunks_total
 * is 0 (single-stream mode).
 */
std::string format_progress_line(const ProgressSample& s);

/*
 * Final summary line, e.g.
 * "done 834.0MiB in 16s (avg 52.1MiB/s) sha256 <digest>".
 * The "sha256 <digest>" part is omitted when the digest is empty.
 */
std::string format_summary(long long done_bytes, long long elapsed_ms,
						   double avg_bytes_per_sec, const std::string& sha256);

/* Human-readable byte count: B/KiB/MiB/GiB, one decimal, base 1024. */
std::string format_bytes(long long n);

} // namespace ferry

#endif // FERRY_CLIENT_PROGRESS_H
