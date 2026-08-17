#ifndef FERRY_BACKOFF_H
#define FERRY_BACKOFF_H

#include <string>

namespace ferry
{

/*
 * Classification of one chunk attempt's outcome, driving the engine's
 * retry policy.
 */
enum class ChunkOutcome
{
	SUCCESS,			/* 206 with matching Content-Range */
	COMPLETE_416,		/* 416: offset already at EOF, chunk is complete */
	RATE_LIMITED,		/* 429: wait max(Retry-After, backoff) and retry */
	FATAL,				/* 403/404: stop all workers, keep .part */
	MISMATCH,			/* any other status: unexpected response shape */
	TRANSIENT			/* network-level failure (status 0) or 5xx: retry */
};

/*
 * `http_status` is the HTTP status, or 0 when no HTTP response was
 * received (network-level failure).
 */
ChunkOutcome classify_outcome(int http_status);

/* Retry delay for a failed chunk: min(500ms * 2^attempt, 30s). */
long long backoff_ms(int attempt);

/* A chunk is given up once `attempt` (counted from 0) reaches 8. */
bool attempts_exhausted(int attempt);

/*
 * Wait before retrying a 429: max(Retry-After seconds * 1000,
 * backoff_ms(attempt)). An absent or unparseable Retry-After falls
 * back to the backoff value alone.
 */
long long rate_limited_wait_ms(int attempt, const std::string& retry_after);

} // namespace ferry

#endif // FERRY_BACKOFF_H
