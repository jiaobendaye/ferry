#ifndef FERRY_TOKEN_BUCKET_H
#define FERRY_TOKEN_BUCKET_H

#include <chrono>
#include <functional>
#include <mutex>

namespace ferry
{

/*
 * Single token bucket with lazy refill. Thread-safe: reserve() is
 * guarded by an internal mutex, so one bucket can be shared by all
 * handler threads (the global gates do exactly that). The time source
 * is injectable:
 * production passes the default (steady_clock), unit tests pass a fake
 * clock and advance it exactly — no sleeping in tests.
 *
 * rate <= 0 disables limiting entirely (reserve always grants).
 * capacity is one second worth of tokens.
 *
 * reserve() charges on grant (possibly into negative; the subsequent wait
 * accrues against it). On rejection no state is changed. Units are
 * caller-defined: bytes for bandwidth limiting, 1 per request for QPS.
 */
class TokenBucket
{
public:
	using TimePoint = std::chrono::steady_clock::time_point;
	using Clock = std::function<TimePoint()>;
	using Millis = std::chrono::milliseconds;

	struct Verdict
	{
		bool rejected = false;	/* true -> reject with 429 */
		Millis wait{0};			/* delay before serving (0 = now) */
	};

	explicit TokenBucket(long long rate, long long max_wait_sec,
						 Clock clock = std::chrono::steady_clock::now);

	/*
	 * Reserve `tokens`. tokens <= 0 is a no-op grant (nothing to charge).
	 */
	Verdict reserve(long long tokens);

	bool disabled() const { return this->rate_ <= 0; }

	long long rate() const { return this->rate_; }
	long long max_wait_ms() const { return this->max_wait_.count(); }

private:
	long long rate_;
	Millis max_wait_;
	Clock clock_;
	double capacity_;			/* 1 second worth of tokens */
	double tokens_;
	TimePoint last_refill_;
	mutable std::mutex mutex_;
};

} // namespace ferry

#endif // FERRY_TOKEN_BUCKET_H
