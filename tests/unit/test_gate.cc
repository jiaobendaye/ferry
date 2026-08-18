#include <memory>
#include <string>
#include <vector>
#include <gtest/gtest.h>

#include "config.h"
#include "gate.h"
#include "gates.h"

namespace
{

/* ---------------- ReleaseList ---------------- */

TEST(ReleaseList, RunsInReverseOrderOnDestroy)
{
	std::vector<int> order;

	{
		ferry::ReleaseList rl;
		rl.add([&] { order.push_back(1); });
		rl.add([&] { order.push_back(2); });
		rl.add([&] { order.push_back(3); });
		EXPECT_EQ(order.size(), 0u);
	}

	ASSERT_EQ(order.size(), 3u);
	EXPECT_EQ(order[0], 3);
	EXPECT_EQ(order[1], 2);
	EXPECT_EQ(order[2], 1);
}

TEST(ReleaseList, MoveTransfersOwnershipWithoutDoubleRelease)
{
	int count = 0;
	ferry::ReleaseList source;
	source.add([&] { count++; });
	source.add([&] { count++; });

	ferry::ReleaseList dest;
	dest = std::move(source);			/* source gives up ownership */
	EXPECT_EQ(count, 0);

	/* source destruction must not run the moved-away obligations */
	{ ferry::ReleaseList tmp(std::move(source)); }
	EXPECT_EQ(count, 0);

	dest.run_all();
	EXPECT_EQ(count, 2);

	dest.run_all();						/* idempotent */
	EXPECT_EQ(count, 2);
}

TEST(ReleaseList, AppendMergesAndDrainsSource)
{
	std::vector<int> order;

	ferry::ReleaseList a;
	a.add([&] { order.push_back(1); });
	ferry::ReleaseList b;
	b.add([&] { order.push_back(2); });
	b.add([&] { order.push_back(3); });

	a.append(std::move(b));
	EXPECT_TRUE(b.empty());
	EXPECT_EQ(a.size(), 3u);

	a.run_all();
	ASSERT_EQ(order.size(), 3u);
	EXPECT_EQ(order[0], 3);				/* overall reverse order */
	EXPECT_EQ(order[1], 2);
	EXPECT_EQ(order[2], 1);
}

/* ---------------- GateChain with stub gates ---------------- */

class StubGate : public ferry::Gate
{
public:
	StubGate(const char *name, ferry::Stats::GateId id,
			 bool reject = false, long long wait_ms = 0,
			 bool acquire = false, std::vector<int> *release_log = nullptr)
		: name_(name), id_(id), reject_(reject), wait_ms_(wait_ms),
		  acquire_(acquire), release_log_(release_log)
	{
	}

	const char *name() const override { return this->name_; }
	ferry::Stats::GateId id() const override { return this->id_; }

	ferry::GateVerdict check(ferry::GateCtx&) override
	{
		ferry::GateVerdict v;
		if (this->reject_)
		{
			v.rejected = true;
			v.status = "429";
			v.retry_after_sec = 7;
			return v;
		}
		v.wait = std::chrono::milliseconds(this->wait_ms_);
		if (this->acquire_)
		{
			auto *log = this->release_log_;
			const char *name = this->name_;
			v.release = [log, name]() {
				log->push_back(name[0]);	/* identify by first char */
			};
		}
		return v;
	}

private:
	const char *name_;
	ferry::Stats::GateId id_;
	bool reject_;
	long long wait_ms_;
	bool acquire_;
	std::vector<int> *release_log_;
};

TEST(GateChain, EmptyChainPasses)
{
	ferry::GateChain chain;
	ferry::GateCtx ctx;
	auto r = chain.run(ctx, nullptr);
	EXPECT_FALSE(r.rejected);
	EXPECT_EQ(r.delay.count(), 0);
	EXPECT_TRUE(r.releases.empty());
}

TEST(GateChain, DelaysComposeByMax)
{
	ferry::GateChain chain;
	chain.add(std::make_unique<StubGate>("a", ferry::Stats::GATE_QPS_TOTAL,
										 false, 200));
	chain.add(std::make_unique<StubGate>("b", ferry::Stats::GATE_RATE_TOTAL,
										 false, 5000));
	chain.add(std::make_unique<StubGate>("c", ferry::Stats::GATE_RATE_PER_IP,
										 false, 300));

	ferry::GateCtx ctx;
	auto r = chain.run(ctx, nullptr);
	EXPECT_FALSE(r.rejected);
	EXPECT_EQ(r.delay.count(), 5000);	/* max, not sum */
}

TEST(GateChain, MidChainRejectRollsBackAndCounts)
{
	std::vector<int> released;
	ferry::Stats stats;

	ferry::GateChain chain;
	chain.add(std::make_unique<StubGate>("a1", ferry::Stats::GATE_QPS_TOTAL,
										 false, 0, true, &released));
	chain.add(std::make_unique<StubGate>("b2", ferry::Stats::GATE_INFLIGHT,
										 false, 0, true, &released));
	chain.add(std::make_unique<StubGate>("c3", ferry::Stats::GATE_QPS_PER_IP,
										 true));	/* rejects */
	chain.add(std::make_unique<StubGate>("d4", ferry::Stats::GATE_RATE_TOTAL,
										 false, 0, true, &released));

	ferry::GateCtx ctx;
	auto r = chain.run(ctx, &stats);

	ASSERT_TRUE(r.rejected);
	EXPECT_STREQ(r.status, "429");
	EXPECT_EQ(r.retry_after_sec, 7);

	/* gates a1 and b2 were acquired -> rolled back in reverse */
	ASSERT_EQ(released.size(), 2u);
	EXPECT_EQ(released[0], 'b');
	EXPECT_EQ(released[1], 'a');
	EXPECT_TRUE(r.releases.empty());	/* nothing handed off */

	/* rejecting gate counted; gate d4 never ran */
	auto s = stats.snapshot();
	EXPECT_EQ(s.gate_rejects[ferry::Stats::GATE_QPS_PER_IP], 1);
	EXPECT_EQ(s.gate_rejects[ferry::Stats::GATE_QPS_TOTAL], 0);
}

TEST(GateChain, PassHandsOffReleases)
{
	std::vector<int> released;
	ferry::GateChain chain;
	chain.add(std::make_unique<StubGate>("a1", ferry::Stats::GATE_QPS_TOTAL,
										 false, 0, true, &released));
	chain.add(std::make_unique<StubGate>("b2", ferry::Stats::GATE_INFLIGHT,
										 false, 0, true, &released));

	ferry::GateCtx ctx;
	auto r = chain.run(ctx, nullptr);

	ASSERT_FALSE(r.rejected);
	EXPECT_EQ(released.size(), 0u);		/* not yet: handed off */
	EXPECT_EQ(r.releases.size(), 2u);

	r.releases.run_all();				/* simulate response completion */
	ASSERT_EQ(released.size(), 2u);
	EXPECT_EQ(released[0], 'b');
	EXPECT_EQ(released[1], 'a');
}

/* ---------------- TokenBucketGate ---------------- */

struct FakeClock
{
	ferry::TokenBucket::TimePoint now;
	ferry::TokenBucket::TimePoint operator()() { return now; }
	void advance_ms(long long ms) { now += std::chrono::milliseconds(ms); }
};

TEST(TokenBucketGate, GlobalQpsChargesOneUnitPerRequest)
{
	FakeClock clock;
	auto bucket = std::make_shared<ferry::TokenBucket>(
					2, 30, [&] { return clock(); });	/* 2 req/s */
	ferry::TokenBucketGate gate("qps_total", ferry::Stats::GATE_QPS_TOTAL,
								bucket, nullptr, false, 30);

	std::string ip = "10.0.0.1";
	ferry::GateCtx ctx;
	ctx.ip_key = &ip;

	EXPECT_EQ(gate.check(ctx).wait.count(), 0);	/* burst: 2 requests */
	EXPECT_EQ(gate.check(ctx).wait.count(), 0);

	auto v = gate.check(ctx);					/* 3rd: deficit 1 at 2/s */
	EXPECT_FALSE(v.rejected);
	EXPECT_NEAR(v.wait.count(), 500, 1);
}

TEST(TokenBucketGate, BandwidthModeChargesCtxBytes)
{
	FakeClock clock;
	auto bucket = std::make_shared<ferry::TokenBucket>(
					1000, 30, [&] { return clock(); });	/* 1000 units/s */
	ferry::TokenBucketGate gate("rate_total", ferry::Stats::GATE_RATE_TOTAL,
								bucket, nullptr, true, 30);

	std::string ip = "10.0.0.1";
	ferry::GateCtx ctx;
	ctx.ip_key = &ip;
	ctx.bytes = 1500;							/* deficit 500 -> 500 ms */

	auto v = gate.check(ctx);
	EXPECT_FALSE(v.rejected);
	EXPECT_NEAR(v.wait.count(), 500, 1);

	/* zero-length body: free pass */
	ctx.bytes = 0;
	EXPECT_EQ(gate.check(ctx).wait.count(), 0);
}

TEST(TokenBucketGate, PerIpModeKeysByIpAndRejects429)
{
	FakeClock clock;
	auto limiter = std::make_shared<ferry::RateLimiter>(
					1000, 1, [&] { return clock(); });	/* max_wait 1 s */
	ferry::TokenBucketGate gate("rate_per_ip", ferry::Stats::GATE_RATE_PER_IP,
								nullptr, limiter, true, 1);

	std::string ip1 = "10.0.0.1";
	std::string ip2 = "10.0.0.2";
	ferry::GateCtx ctx;
	ctx.ip_key = &ip1;
	ctx.bytes = 5000;					/* 4000 deficit -> 4 s > 1 s */

	auto v = gate.check(ctx);
	ASSERT_TRUE(v.rejected);
	EXPECT_STREQ(v.status, "429");
	EXPECT_EQ(v.retry_after_sec, 1);

	/* other IP untouched */
	ctx.ip_key = &ip2;
	ctx.bytes = 500;
	EXPECT_FALSE(gate.check(ctx).rejected);
}

/* ---------------- SemaphoreGate ---------------- */

TEST(SemaphoreGate, AcquireAndReleaseViaVerdictClosure)
{
	auto lim = std::make_shared<ferry::ConcurrencyLimiter>(1);
	ferry::SemaphoreGate gate("max_inflight", ferry::Stats::GATE_INFLIGHT,
							  lim, false);

	std::string ip = "10.0.0.1";
	ferry::GateCtx ctx;
	ctx.ip_key = &ip;

	auto v1 = gate.check(ctx);
	ASSERT_FALSE(v1.rejected);
	ASSERT_TRUE(v1.release != nullptr);
	EXPECT_EQ(lim->current(), 1);

	auto v2 = gate.check(ctx);
	ASSERT_TRUE(v2.rejected);
	EXPECT_STREQ(v2.status, "503");
	EXPECT_EQ(v2.retry_after_sec, 1);
	EXPECT_TRUE(v2.release == nullptr);

	v1.release();						/* response completed */
	EXPECT_EQ(lim->current(), 0);
	EXPECT_FALSE(gate.check(ctx).rejected);
}

TEST(SemaphoreGate, KeyedModeKeysByIp)
{
	auto lim = std::make_shared<ferry::ConcurrencyLimiter>(1);
	ferry::SemaphoreGate gate("max_inflight_per_ip",
							  ferry::Stats::GATE_INFLIGHT_PER_IP, lim, true);

	std::string ip1 = "10.0.0.1";
	std::string ip2 = "10.0.0.2";
	ferry::GateCtx ctx;
	ctx.ip_key = &ip1;

	auto v1 = gate.check(ctx);
	ASSERT_FALSE(v1.rejected);

	ctx.ip_key = &ip2;
	EXPECT_FALSE(gate.check(ctx).rejected);	/* ip2 has its own slot */

	ctx.ip_key = &ip1;
	EXPECT_TRUE(gate.check(ctx).rejected);	/* ip1 at cap */

	v1.release();
	EXPECT_EQ(lim->size(), 1u);				/* ip2 still in flight */
}

/* ---------------- build_gate_chains ---------------- */

TEST(BuildGateChains, EmptyConfigProducesEmptyChains)
{
	ferry::ServerConfig cfg;			/* everything off by default */
	auto setup = ferry::build_gate_chains(cfg);

	ASSERT_TRUE(setup.pre);
	ASSERT_TRUE(setup.post);
	EXPECT_TRUE(setup.pre->empty());
	EXPECT_TRUE(setup.post->empty());
	EXPECT_FALSE(setup.qps_per_ip_limiter);
	EXPECT_FALSE(setup.bw_per_ip_limiter);
	EXPECT_FALSE(setup.inflight_limiter);
	EXPECT_FALSE(setup.inflight_per_ip_limiter);
}

TEST(BuildGateChains, FullConfigOrderAndMembership)
{
	ferry::ServerConfig cfg;
	cfg.qps_total = 100;
	cfg.max_inflight = 50;
	cfg.qps_per_ip = 10;
	cfg.max_inflight_per_ip = 4;
	cfg.rate_total_bps = 1000000;
	cfg.rate_bytes_per_sec = 100000;

	auto setup = ferry::build_gate_chains(cfg);

	ASSERT_EQ(setup.pre->size(), 4u);
	EXPECT_STREQ(setup.pre->gate_name(0), "qps_total");
	EXPECT_STREQ(setup.pre->gate_name(1), "max_inflight");
	EXPECT_STREQ(setup.pre->gate_name(2), "qps_per_ip");
	EXPECT_STREQ(setup.pre->gate_name(3), "max_inflight_per_ip");

	ASSERT_EQ(setup.post->size(), 2u);
	EXPECT_STREQ(setup.post->gate_name(0), "rate_total");
	EXPECT_STREQ(setup.post->gate_name(1), "rate_per_ip");

	EXPECT_TRUE(setup.qps_per_ip_limiter != nullptr);
	EXPECT_TRUE(setup.bw_per_ip_limiter != nullptr);
	EXPECT_TRUE(setup.inflight_limiter != nullptr);
	EXPECT_TRUE(setup.inflight_per_ip_limiter != nullptr);
}

TEST(BuildGateChains, PartialConfigOmitsDisabledGates)
{
	ferry::ServerConfig cfg;
	cfg.qps_total = 100;
	cfg.rate_bytes_per_sec = 100000;

	auto setup = ferry::build_gate_chains(cfg);

	ASSERT_EQ(setup.pre->size(), 1u);
	EXPECT_STREQ(setup.pre->gate_name(0), "qps_total");
	ASSERT_EQ(setup.post->size(), 1u);
	EXPECT_STREQ(setup.post->gate_name(0), "rate_per_ip");
	EXPECT_FALSE(setup.qps_per_ip_limiter);
	EXPECT_TRUE(setup.bw_per_ip_limiter != nullptr);
}

} // namespace
