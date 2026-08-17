#include <cerrno>
#include <climits>
#include <cstdlib>
#include "backoff.h"

namespace ferry
{

static const long long BACKOFF_BASE_MS = 500;
static const long long BACKOFF_CAP_MS = 30000;
static const int MAX_ATTEMPTS = 8;

ChunkOutcome classify_outcome(int http_status)
{
	switch (http_status)
	{
	case 206:
		return ChunkOutcome::SUCCESS;
	case 416:
		return ChunkOutcome::COMPLETE_416;
	case 429:
		return ChunkOutcome::RATE_LIMITED;
	case 403:
	case 404:
		return ChunkOutcome::FATAL;
	case 0:
		return ChunkOutcome::TRANSIENT;
	default:
		break;
	}

	if (http_status >= 500 && http_status <= 599)
		return ChunkOutcome::TRANSIENT;

	return ChunkOutcome::MISMATCH;
}

long long backoff_ms(int attempt)
{
	if (attempt < 0)
		attempt = 0;

	/* 500 * 2^6 = 32000 already exceeds the cap. */
	if (attempt >= 6)
		return BACKOFF_CAP_MS;

	long long ms = BACKOFF_BASE_MS << attempt;
	return ms < BACKOFF_CAP_MS ? ms : BACKOFF_CAP_MS;
}

bool attempts_exhausted(int attempt)
{
	return attempt >= MAX_ATTEMPTS;
}

/*
 * Parse `value` as a non-negative delta-seconds count (leading/trailing
 * whitespace tolerated). Returns false when absent or unparseable
 * (HTTP-date forms, garbage, negatives, overflow).
 */
static bool parse_retry_after(const std::string& value, long long *seconds)
{
	size_t b = value.find_first_not_of(" \t");
	if (b == std::string::npos)
		return false;
	size_t e = value.find_last_not_of(" \t");
	std::string s = value.substr(b, e - b + 1);

	const char *p = s.c_str();
	char *end = NULL;

	errno = 0;
	long long v = strtoll(p, &end, 10);
	if (errno != 0 || end == p || *end != '\0' || v < 0)
		return false;

	*seconds = v;
	return true;
}

long long rate_limited_wait_ms(int attempt, const std::string& retry_after)
{
	long long wait = backoff_ms(attempt);

	long long seconds;
	if (parse_retry_after(retry_after, &seconds) && seconds > 0)
	{
		/* Guard the multiplication against absurd header values. */
		long long ms = seconds > LLONG_MAX / 1000 ? LLONG_MAX : seconds * 1000;
		if (ms > wait)
			wait = ms;
	}

	return wait;
}

} // namespace ferry
