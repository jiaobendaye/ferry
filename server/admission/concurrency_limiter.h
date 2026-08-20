#ifndef WF_CONCURRENCY_LIMITER_H
#define WF_CONCURRENCY_LIMITER_H

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ferry
{

/*
 * Counting semaphore for in-flight request caps. Two modes, chosen by
 * which overload is used:
 *
 *  - global: try_acquire()/release() bound the total across all clients
 *    (atomic fast path, no map);
 *  - keyed:  try_acquire(key)/release(key) bound each key (per-IP)
 *    separately. Keyed entries are erased when their count returns to
 *    zero, so the map cannot accumulate idle entries and no sweep is
 *    needed (unlike token buckets, counters drain by themselves).
 *
 * cap <= 0 disables limiting (acquire always grants). An instance is
 * either global or keyed for its lifetime; mixing the overload styles
 * on one instance is not supported.
 */
class ConcurrencyLimiter
{
public:
	explicit ConcurrencyLimiter(int cap);

	bool disabled() const { return this->cap_ <= 0; }
	int cap() const { return this->cap_; }

	/* Global mode. Returns false when the cap is reached. */
	bool try_acquire();
	void release();

	/* Keyed mode. Returns false when the cap is reached for `key`. */
	bool try_acquire(const std::string& key);
	void release(const std::string& key);

	/* Introspection for tests/stats. */
	int current() const { return this->count_.load(); }
	size_t size();						/* keyed entries */
	int current(const std::string& key);	/* 0 when absent */

private:
	int cap_;
	std::atomic<int> count_{0};			/* global mode */
	std::mutex mutex_;					/* keyed mode */
	std::unordered_map<std::string, int> keyed_;
};

} // namespace ferry

#endif // WF_CONCURRENCY_LIMITER_H
