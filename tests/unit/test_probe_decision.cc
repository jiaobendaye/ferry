#include <gtest/gtest.h>
#include <string>
#include "probe.h"

namespace
{

using ferry::DownloadMode;
using ferry::HeadResult;
using ferry::ProbeDecision;
using ferry::ProbeResult;

const long long LIMIT = 256LL * 1024 * 1024;
const char *LM = "Mon, 17 Aug 2026 12:00:00 GMT";

/* HEAD 200 with Content-Length (usable), Accept-Ranges configurable. */
HeadResult head_ok(long long content_length, bool accept_ranges)
{
	HeadResult h;
	h.got_response = true;
	h.status = 200;
	h.content_length = content_length;
	h.accept_ranges = accept_ranges;
	h.last_modified = LM;
	return h;
}

/* HEAD that cannot be relied on: failure, error status, or no size. */
HeadResult head_failed()
{
	HeadResult h;
	h.got_response = false;
	return h;
}

HeadResult head_405()
{
	HeadResult h;
	h.got_response = true;
	h.status = 405;
	h.last_modified = LM;
	return h;
}

HeadResult head_no_length()
{
	HeadResult h;
	h.got_response = true;
	h.status = 200;
	h.content_length = -1;
	h.last_modified = LM;
	return h;
}

ProbeResult probe_not_performed()
{
	return ProbeResult();
}

ProbeResult probe(int status, long long content_length)
{
	ProbeResult p;
	p.performed = true;
	p.got_response = true;
	p.status = status;
	p.content_length = content_length;
	return p;
}

ProbeResult probe_network_failure()
{
	ProbeResult p;
	p.performed = true;
	p.got_response = false;
	return p;
}

ProbeDecision decide(const HeadResult& h, const ProbeResult& p,
					 long long limit = LIMIT)
{
	return ferry::decide_mode(h, p, limit);
}

/* ---------------- HEAD usable ---------------- */

TEST(ProbeDecision, AcceptRangesGivesChunkWithoutProbe)
{
	auto d = decide(head_ok(1000, true), probe_not_performed());
	EXPECT_EQ(d.mode, DownloadMode::CHUNK);
	EXPECT_EQ(d.known_size, 1000);
	EXPECT_EQ(d.last_modified, LM);
	EXPECT_FALSE(d.probe_body_is_download_start);
}

TEST(ProbeDecision, NoAcceptRangesWithoutProbeFails)
{
	auto d = decide(head_ok(1000, false), probe_not_performed());
	EXPECT_EQ(d.mode, DownloadMode::FAILED);
}

TEST(ProbeDecision, NoAcceptRangesProbe206GivesChunk)
{
	auto d = decide(head_ok(1000, false), probe(206, 1));
	EXPECT_EQ(d.mode, DownloadMode::CHUNK);
	EXPECT_EQ(d.known_size, 1000);			/* from HEAD, not the 206 body */
	EXPECT_EQ(d.last_modified, LM);
	EXPECT_FALSE(d.probe_body_is_download_start);
}

TEST(ProbeDecision, NoAcceptRangesProbe200WithinLimit)
{
	auto d = decide(head_ok(1000, false), probe(200, 1000));
	EXPECT_EQ(d.mode, DownloadMode::SINGLE_STREAM);
	EXPECT_EQ(d.known_size, 1000);
	EXPECT_TRUE(d.probe_body_is_download_start);
}

TEST(ProbeDecision, NoAcceptRangesProbe200AtLimitAllowed)
{
	auto d = decide(head_ok(LIMIT, false), probe(200, LIMIT));
	EXPECT_EQ(d.mode, DownloadMode::SINGLE_STREAM);
	EXPECT_EQ(d.known_size, LIMIT);
	EXPECT_TRUE(d.probe_body_is_download_start);
}

TEST(ProbeDecision, NoAcceptRangesProbe200OverLimitRefused)
{
	auto d = decide(head_ok(LIMIT + 1, false), probe(200, LIMIT + 1));
	EXPECT_EQ(d.mode, DownloadMode::REFUSE_OVERSIZE);
	EXPECT_EQ(d.known_size, LIMIT + 1);
	EXPECT_FALSE(d.probe_body_is_download_start);
}

TEST(ProbeDecision, NoAcceptRangesProbeFailedFails)
{
	auto d = decide(head_ok(1000, false), probe_network_failure());
	EXPECT_EQ(d.mode, DownloadMode::FAILED);
}

TEST(ProbeDecision, NoAcceptRangesProbeUnexpectedStatusFails)
{
	auto d = decide(head_ok(1000, false), probe(404, 0));
	EXPECT_EQ(d.mode, DownloadMode::FAILED);
}

/* ---------------- HEAD not usable ---------------- */

TEST(ProbeDecision, HeadFailedProbe206GivesChunkWithTotal)
{
	auto d = decide(head_failed(), probe(206, 5000));
	EXPECT_EQ(d.mode, DownloadMode::CHUNK);
	EXPECT_EQ(d.known_size, 5000);			/* Content-Range total */
	EXPECT_FALSE(d.probe_body_is_download_start);
}

TEST(ProbeDecision, HeadFailedProbe206UnknownTotal)
{
	auto d = decide(head_failed(), probe(206, -1));
	EXPECT_EQ(d.mode, DownloadMode::CHUNK);
	EXPECT_EQ(d.known_size, -1);
}

TEST(ProbeDecision, Head405Probe200GivesSingleStream)
{
	auto d = decide(head_405(), probe(200, 700));
	EXPECT_EQ(d.mode, DownloadMode::SINGLE_STREAM);
	EXPECT_EQ(d.known_size, 700);
	EXPECT_TRUE(d.probe_body_is_download_start);
}

TEST(ProbeDecision, Head405Probe200AtLimitAllowed)
{
	auto d = decide(head_405(), probe(200, LIMIT));
	EXPECT_EQ(d.mode, DownloadMode::SINGLE_STREAM);
	EXPECT_EQ(d.known_size, LIMIT);
}

TEST(ProbeDecision, Head405Probe200OverLimitRefused)
{
	auto d = decide(head_405(), probe(200, LIMIT + 1));
	EXPECT_EQ(d.mode, DownloadMode::REFUSE_OVERSIZE);
	EXPECT_EQ(d.known_size, LIMIT + 1);
	EXPECT_FALSE(d.probe_body_is_download_start);
}

TEST(ProbeDecision, Head405Probe200UnknownSizeStreams)
{
	/* unknown size enters single-stream; the engine enforces the limit */
	auto d = decide(head_405(), probe(200, -1));
	EXPECT_EQ(d.mode, DownloadMode::SINGLE_STREAM);
	EXPECT_EQ(d.known_size, -1);
	EXPECT_TRUE(d.probe_body_is_download_start);
}

TEST(ProbeDecision, HeadNoContentLengthProbe206GivesChunk)
{
	auto d = decide(head_no_length(), probe(206, 4096));
	EXPECT_EQ(d.mode, DownloadMode::CHUNK);
	EXPECT_EQ(d.known_size, 4096);
	EXPECT_EQ(d.last_modified, LM);			/* carried when available */
}

TEST(ProbeDecision, HeadFailedProbeSkippedFails)
{
	auto d = decide(head_failed(), probe_not_performed());
	EXPECT_EQ(d.mode, DownloadMode::FAILED);
}

TEST(ProbeDecision, HeadFailedProbeNetworkFailureFails)
{
	auto d = decide(head_failed(), probe_network_failure());
	EXPECT_EQ(d.mode, DownloadMode::FAILED);
}

TEST(ProbeDecision, HeadFailedProbeErrorStatusFails)
{
	EXPECT_EQ(decide(head_failed(), probe(500, 0)).mode, DownloadMode::FAILED);
	EXPECT_EQ(decide(head_405(), probe(403, 0)).mode, DownloadMode::FAILED);
	EXPECT_EQ(decide(head_failed(), probe(416, 0)).mode, DownloadMode::FAILED);
}

/* ---------------- boundary on the limit itself ---------------- */

TEST(ProbeDecision, LimitBoundaryIndependentOfHead)
{
	/* size == limit is allowed, size == limit + 1 is refused */
	long long limit = 1000;

	auto ok1 = decide(head_ok(1000, false), probe(200, 1000), limit);
	EXPECT_EQ(ok1.mode, DownloadMode::SINGLE_STREAM);

	auto bad1 = decide(head_ok(1001, false), probe(200, 1001), limit);
	EXPECT_EQ(bad1.mode, DownloadMode::REFUSE_OVERSIZE);

	auto ok2 = decide(head_failed(), probe(200, 1000), limit);
	EXPECT_EQ(ok2.mode, DownloadMode::SINGLE_STREAM);

	auto bad2 = decide(head_failed(), probe(200, 1001), limit);
	EXPECT_EQ(bad2.mode, DownloadMode::REFUSE_OVERSIZE);
}

} // namespace
