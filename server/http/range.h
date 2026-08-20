#ifndef FERRY_RANGE_H
#define FERRY_RANGE_H

#include <string>

namespace ferry
{

/*
 * Decision for one request, computed from the Range header, the file size
 * and the configured cap/threshold.
 *
 * status 200: serve the whole file (offset 0, length file_size).
 * status 206: serve [offset, offset + length - 1], length <= cap.
 * status 413: non-Range request for a file larger than the threshold.
 * status 416: unsatisfiable range (start >= file_size).
 */
struct RangeDecision
{
	int status = 200;
	long long offset = 0;
	long long length = 0;
};

/*
 * `range_header` is the raw Range header value ("" when absent).
 * Syntactically invalid headers and unsupported units are ignored
 * (treated as no Range). Multi-range requests keep the first range only.
 */
RangeDecision decide_range(const std::string& range_header,
						   long long file_size,
						   long long cap_bytes,
						   long long threshold_bytes);

} // namespace ferry

#endif // FERRY_RANGE_H
