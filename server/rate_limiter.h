#ifndef WF_RATE_LIMITER_H
#define WF_RATE_LIMITER_H

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ferry
{

/*
 * Per-IP bandwidth limiter (token bucket, bytes/sec). The time source is
 * injectable: production passes the default (steady_clock), unit tests
 * pass a fake clock and advance it exactly — no sleeping in tests.
 *
 * rate_bps == 0 disables limiting entirely (reserve always grants).
 */
class RateLimiter
{
public:
	using TimePoint = std::chrono::steady_clock::time_point;
	using Clock = std::function<TimePoint()>;
	using Millis = std::chrono::milliseconds;

	struct Verdict
	{
		bool rejected = false;	/* true -> reply 429 */
		Millis wait{0};			/* delay before serving (0 = now) */
	};

	explicit RateLimiter(long long rate_bps, long long max_wait_sec,
						 Clock clock = std::chrono::steady_clock::now);

	/*
	 * Reserve `bytes` for `ip_key`. Charges the bucket (possibly into
	 * negative) when the request is granted, so the subsequent wait
	 * accrues against it. On rejection no state is changed.
	 */
	Verdict reserve(const std::string& ip_key, long long bytes);

	/* Drop buckets idle for longer than `idle_threshold`. */
	void sweep(Millis idle_threshold);

	bool disabled() const { return this->rate_bps_ <= 0; }

	size_t size();						/* active buckets (for tests) */

private:
	struct Bucket
	{
		double tokens;
		TimePoint last_refill;
		TimePoint last_active;
	};

	long long rate_bps_;
	Millis max_wait_;
	Clock clock_;
	double capacity_;					/* 1 second worth of tokens */
	std::mutex mutex_;
	std::unordered_map<std::string, Bucket> buckets_;
};

} // namespace ferry

#endif // WF_RATE_LIMITER_H
