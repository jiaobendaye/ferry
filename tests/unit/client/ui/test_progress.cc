#include <string>
#include <gtest/gtest.h>
#include "ui/progress.h"

namespace
{

TEST(Progress, FormatBytesUnitsAndBoundaries)
{
	EXPECT_EQ(ferry::format_bytes(0), "0.0B");
	EXPECT_EQ(ferry::format_bytes(1), "1.0B");
	EXPECT_EQ(ferry::format_bytes(1023), "1023.0B");
	EXPECT_EQ(ferry::format_bytes(1024), "1.0KiB");
	EXPECT_EQ(ferry::format_bytes(1536), "1.5KiB");
	EXPECT_EQ(ferry::format_bytes(1048575), "1024.0KiB");
	EXPECT_EQ(ferry::format_bytes(1048576), "1.0MiB");
	EXPECT_EQ(ferry::format_bytes(834LL * 1024 * 1024), "834.0MiB");
	EXPECT_EQ(ferry::format_bytes(1073741824LL), "1.0GiB");
	EXPECT_EQ(ferry::format_bytes(5LL * 1024 * 1024 * 1024), "5.0GiB");
	EXPECT_EQ(ferry::format_bytes(2048LL * 1024 * 1024 * 1024), "2048.0GiB");
}

TEST(Progress, ProgressLineKnownSize)
{
	ferry::ProgressSample s;
	s.percent = 45.2;
	s.done_bytes = 378LL * 1024 * 1024;
	s.total_bytes = 834LL * 1024 * 1024;
	s.bytes_per_sec = 52.3 * 1024 * 1024;
	s.eta_sec = 9;
	s.chunks_done = 48;
	s.chunks_total = 105;
	s.retries = 3;

	std::string line = ferry::format_progress_line(s);
	EXPECT_EQ(line, "45.2%  378.0MiB/834.0MiB  52.3MiB/s"
					"  ETA 9s  chunks 48/105  retries 3");

	/* the individual pieces are all present */
	EXPECT_NE(line.find("45.2%"), std::string::npos);
	EXPECT_NE(line.find("378.0MiB/834.0MiB"), std::string::npos);
	EXPECT_NE(line.find("52.3MiB/s"), std::string::npos);
	EXPECT_NE(line.find("ETA 9s"), std::string::npos);
	EXPECT_NE(line.find("chunks 48/105"), std::string::npos);
	EXPECT_NE(line.find("retries 3"), std::string::npos);
}

TEST(Progress, ProgressLineUnknownSizeOmitsPercentAndEta)
{
	ferry::ProgressSample s;
	s.percent = -1;
	s.done_bytes = 378LL * 1024 * 1024;
	s.total_bytes = 0;
	s.bytes_per_sec = 52.3 * 1024 * 1024;
	s.retries = 1;

	std::string line = ferry::format_progress_line(s);
	EXPECT_EQ(line, "378.0MiB/??  52.3MiB/s  retries 1");

	EXPECT_NE(line.find("/??"), std::string::npos);
	EXPECT_EQ(line.find('%'), std::string::npos);
	EXPECT_EQ(line.find("ETA"), std::string::npos);
	EXPECT_EQ(line.find("chunks"), std::string::npos);
}

TEST(Progress, SummaryContent)
{
	std::string line = ferry::format_summary(834LL * 1024 * 1024, 16000,
											 52.1 * 1024 * 1024,
											 "e3b0c44298fc1c14");
	EXPECT_EQ(line, "done 834.0MiB in 16s (avg 52.1MiB/s)"
					" sha256 e3b0c44298fc1c14");
}

TEST(Progress, SummaryRoundsElapsedAndOmitsEmptyDigest)
{
	EXPECT_EQ(ferry::format_summary(1024, 500, 2048, ""),
			  "done 1.0KiB in 1s (avg 2.0KiB/s)");
	EXPECT_EQ(ferry::format_summary(0, 0, 0, ""),
			  "done 0.0B in 0s (avg 0.0B/s)");
	EXPECT_EQ(ferry::format_summary(2048, 16499, 4096, "ab"),
			  "done 2.0KiB in 16s (avg 4.0KiB/s) sha256 ab");
	EXPECT_EQ(ferry::format_summary(2048, 16500, 4096, "ab"),
			  "done 2.0KiB in 17s (avg 4.0KiB/s) sha256 ab");
}

} // namespace
