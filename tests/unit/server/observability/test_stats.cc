#include <string>
#include <gtest/gtest.h>
#include "observability/stats.h"

namespace
{

TEST(Stats, StatusClassification)
{
	ferry::Stats st;

	st.record_request("200", 100);
	st.record_request("206", 50);
	st.record_request("404", 0);
	st.record_request("400", 0);
	st.record_request("429", 0);
	st.record_request("503", 0);

	auto s = st.snapshot();
	EXPECT_EQ(s.requests, 6);
	EXPECT_EQ(s.status_2xx, 2);
	EXPECT_EQ(s.status_404, 1);
	EXPECT_EQ(s.status_4xx_other, 2);	/* 400 + 429 */
	EXPECT_EQ(s.status_5xx, 1);
	EXPECT_EQ(s.bytes_served, 150);
}

TEST(Stats, ReconciliationInvariant)
{
	ferry::Stats st;
	const char *codes[] = {"200", "206", "400", "403", "404",
						   "405", "413", "416", "429", "503"};

	for (int i = 0; i < 100; i++)
		st.record_request(codes[i % 10], 0);

	auto s = st.snapshot();
	EXPECT_EQ(s.requests,
			  s.status_2xx + s.status_404 + s.status_4xx_other + s.status_5xx);
}

TEST(Stats, BytesCountedOnlyForSuccesses)
{
	ferry::Stats st;

	st.record_request("206", 1000);
	st.record_request("404", 999);		/* error body bytes are not file data */
	st.record_request("200", 0);		/* HEAD: no body */

	EXPECT_EQ(st.snapshot().bytes_served, 1000);
}

TEST(Stats, GateRejectCountersAreIndependent)
{
	ferry::Stats st;

	st.record_gate_reject(ferry::Stats::GATE_QPS_TOTAL);
	st.record_gate_reject(ferry::Stats::GATE_QPS_TOTAL);
	st.record_gate_reject(ferry::Stats::GATE_INFLIGHT);

	auto s = st.snapshot();
	EXPECT_EQ(s.gate_rejects[ferry::Stats::GATE_QPS_TOTAL], 2);
	EXPECT_EQ(s.gate_rejects[ferry::Stats::GATE_INFLIGHT], 1);
	EXPECT_EQ(s.gate_rejects[ferry::Stats::GATE_QPS_PER_IP], 0);
}

TEST(Stats, InflightGaugeAndPeak)
{
	ferry::Stats st;

	st.inflight_inc();
	st.inflight_inc();
	st.inflight_inc();
	EXPECT_EQ(st.snapshot().inflight, 3);
	EXPECT_EQ(st.snapshot().inflight_peak, 3);

	st.inflight_dec();
	st.inflight_dec();
	EXPECT_EQ(st.snapshot().inflight, 1);
	EXPECT_EQ(st.snapshot().inflight_peak, 3);	/* peak never shrinks */

	st.inflight_dec();
	EXPECT_EQ(st.snapshot().inflight, 0);
}

TEST(Stats, MmapGaugePeakAndFallback)
{
	ferry::Stats st;

	st.mmap_open(4096);
	st.mmap_open(8192);
	st.mmap_fallback();
	auto active = st.snapshot();
	EXPECT_EQ(active.mmap_responses, 2);
	EXPECT_EQ(active.mmap_bytes, 12288);
	EXPECT_EQ(active.mmap_active_bytes, 12288);
	EXPECT_EQ(active.mmap_active_peak, 12288);
	EXPECT_EQ(active.mmap_fallbacks, 1);

	st.mmap_close(4096);
	st.mmap_close(8192);
	auto closed = st.snapshot();
	EXPECT_EQ(closed.mmap_active_bytes, 0);
	EXPECT_EQ(closed.mmap_active_peak, 12288);
}

TEST(Stats, FormatLineContainsKeyFieldsAndDeltas)
{
	ferry::Stats st;

	for (int i = 0; i < 10; i++)
		st.record_request("200", 100);

	ferry::Stats::Snapshot prev;	/* all zeros */
	auto cur = st.snapshot();
	std::string line = ferry::format_stats_line(cur, prev, 3, 4);

	EXPECT_NE(line.find("[stats]"), std::string::npos);
	EXPECT_NE(line.find("reqs=10(+10)"), std::string::npos);
	EXPECT_NE(line.find("2xx=10"), std::string::npos);
	EXPECT_NE(line.find("buckets(qps)=3"), std::string::npos);
	EXPECT_NE(line.find("buckets(bw)=4"), std::string::npos);
	EXPECT_NE(line.find("served=1000"), std::string::npos);
	EXPECT_NE(line.find("rej(qps_total)=0"), std::string::npos);
	EXPECT_NE(line.find("mmap_resps=0"), std::string::npos);
	EXPECT_NE(line.find("mmap_fallbacks=0"), std::string::npos);
	EXPECT_EQ(line.find('\n'), std::string::npos);	/* single line */

	/* second interval: delta only */
	st.record_request("429", 0);
	auto cur2 = st.snapshot();
	std::string line2 = ferry::format_stats_line(cur2, cur, 0, 0);
	EXPECT_NE(line2.find("reqs=11(+1)"), std::string::npos);
	EXPECT_NE(line2.find("rej(qps_total)=0"), std::string::npos);
}

} // namespace
