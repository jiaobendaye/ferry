#ifndef FERRY_PROBE_H
#define FERRY_PROBE_H

#include <string>

namespace ferry
{

/*
 * Outcome of the HEAD request. got_response == false means a
 * network-level failure or an otherwise unusable HEAD.
 */
struct HeadResult
{
	bool got_response = false;
	int status = 0;					/* e.g. 200, 405 */
	long long content_length = -1;	/* -1 when absent */
	bool accept_ranges = false;
	std::string last_modified;
};

/*
 * Outcome of the probe GET with `Range: bytes=0-0`.
 * content_length is valid only for non-206 outcomes (a 206 body is one
 * byte); for a 206 the engine fills content_length with the Content-Range
 * total, which may stay -1 when the server omitted it.
 */
struct ProbeResult
{
	bool performed = false;			/* false when the probe GET was skipped */
	bool got_response = false;
	int status = 0;					/* 206 or 200 */
	long long content_length = -1;
};

enum class DownloadMode { CHUNK, SINGLE_STREAM, REFUSE_OVERSIZE, FAILED };

struct ProbeDecision
{
	DownloadMode mode = DownloadMode::FAILED;
	long long known_size = -1;		/* expected total size when known */
	std::string last_modified;
	bool probe_body_is_download_start = false;	/* true: the probe 200 body
												 * is the full file, keep it */
};

/*
 * The complete decision matrix (design D3). Pure function, no IO.
 * `single_stream_limit` is the maximum number of bytes allowed in
 * single-stream mode; a known size above it refuses the download.
 * last_modified is carried from HEAD whenever HEAD supplied one.
 */
ProbeDecision decide_mode(const HeadResult& head, const ProbeResult& probe,
						  long long single_stream_limit);

} // namespace ferry

#endif // FERRY_PROBE_H
