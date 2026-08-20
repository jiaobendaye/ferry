#ifndef FERRY_RATE_LIMITER_H
#define FERRY_RATE_LIMITER_H

#include <mutex>
#include <string>
#include <unordered_map>

#include "token_bucket.h"

namespace ferry
{

/*
 * Per-key token-bucket limiter (a keyed map of TokenBucket). Used for
 * per-IP budgets: bytes/sec bandwidth limiting and requests/sec QPS
 * limiting (units are caller-defined; see TokenBucket). The time source
 * is injectable: production passes the default (steady_clock), unit
 * tests pass a fake clock and advance it exactly — no sleeping in tests.
 *
 * rate_bps == 0 disables limiting entirely (reserve always grants).
 */
class RateLimiter
{
public:
	using TimePoint = TokenBucket::TimePoint;
	using Clock = TokenBucket::Clock;
	using Millis = TokenBucket::Millis;
	using Verdict = TokenBucket::Verdict;

	explicit RateLimiter(long long rate_bps, long long max_wait_sec,
						 Clock clock = std::chrono::steady_clock::now);

	/*
	 * Reserve `tokens` for `ip_key`. Creates a fresh bucket (one second
	 * of burst) on first use. On rejection no state is changed.
	 */
	Verdict reserve(const std::string& ip_key, long long tokens);

	/* Drop buckets idle for longer than `idle_threshold`. */
	void sweep(Millis idle_threshold);

	bool disabled() const { return this->rate_bps_ <= 0; }

	size_t size();						/* active buckets (for tests) */

private:
	/* TokenBucket is non-movable (holds a mutex), so Entry is
	   constructed in place inside the map node. */
	struct Entry
	{
		TokenBucket bucket;
		TimePoint last_active;

		Entry(long long rate, long long max_wait_sec, const Clock& clock,
			  TimePoint now)
			: bucket(rate, max_wait_sec, clock), last_active(now)
		{
		}
	};

	long long rate_bps_;
	long long max_wait_sec_;
	Clock clock_;
	std::mutex mutex_;
	std::unordered_map<std::string, Entry> buckets_;
};

} // namespace ferry

#endif // FERRY_RATE_LIMITER_H
