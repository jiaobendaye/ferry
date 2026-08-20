#ifndef FERRY_GATE_H
#define FERRY_GATE_H

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "observability/stats.h"

namespace ferry
{

/*
 * RAII list of release obligations (semaphore slots etc.). Destroyed
 * before it is handed off -> everything acquired so far is released in
 * reverse order (rollback on mid-chain rejection). Moved into the
 * request state on success -> released exactly once at response
 * completion.
 */
class ReleaseList
{
public:
	using Fn = std::function<void()>;

	ReleaseList() = default;
	ReleaseList(ReleaseList&&) = default;
	ReleaseList& operator=(ReleaseList&&) = default;
	ReleaseList(const ReleaseList&) = delete;
	ReleaseList& operator=(const ReleaseList&) = delete;
	~ReleaseList() { this->run_all(); }

	void add(Fn fn) { this->fns_.push_back(std::move(fn)); }

	/* Move every obligation from `other` onto this list. */
	void append(ReleaseList&& other);

	bool empty() const { return this->fns_.empty(); }
	size_t size() const { return this->fns_.size(); }

	/* Run all obligations in reverse order; idempotent (empties). */
	void run_all();

private:
	std::vector<Fn> fns_;
};

/*
 * What a gate needs to decide. `ip_key` is valid from the pre-chain on;
 * `bytes` (response length) only after the Range decision, i.e. in the
 * post-chain.
 */
struct GateCtx
{
	const std::string *ip_key = nullptr;
	long long bytes = 0;
};

struct GateVerdict
{
	bool rejected = false;
	const char *status = nullptr;		/* "429" / "503" when rejected */
	int retry_after_sec = 0;
	std::chrono::milliseconds wait{0};	/* shaper: delay before serving */
	ReleaseList::Fn release;			/* semaphore: release obligation */
};

/*
 * Admission gate. check() must be synchronous and non-blocking.
 * name() doubles as the human-readable identity in logs; id() indexes
 * the rejection counter in Stats.
 */
class Gate
{
public:
	virtual ~Gate() = default;

	virtual const char *name() const = 0;
	virtual Stats::GateId id() const = 0;
	virtual GateVerdict check(GateCtx& ctx) = 0;
};

struct ChainResult
{
	bool rejected = false;
	const char *status = nullptr;
	int retry_after_sec = 0;
	std::chrono::milliseconds delay{0};
	ReleaseList releases;	/* rolled back already when rejected */
};

/*
 * Ordered list of gates. run():
 *  - first rejection short-circuits; everything acquired from earlier
 *    gates is rolled back (RAII) and the rejecting gate is counted in
 *    Stats;
 *  - shaper waits compose as max (all buckets were charged at the same
 *    instant, so the request is ready when the strictest one is);
 *  - on pass, acquired release obligations are moved into the result.
 */
class GateChain
{
public:
	void add(std::unique_ptr<Gate> gate)
	{
		this->gates_.push_back(std::move(gate));
	}

	bool empty() const { return this->gates_.empty(); }
	size_t size() const { return this->gates_.size(); }
	const char *gate_name(size_t i) const { return this->gates_[i]->name(); }

	ChainResult run(GateCtx& ctx, Stats *stats);

private:
	std::vector<std::unique_ptr<Gate>> gates_;
};

} // namespace ferry

#endif // FERRY_GATE_H
