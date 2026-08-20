#ifndef FERRY_HTTP_PATH_H
#define FERRY_HTTP_PATH_H

#include <string>

namespace ferry
{

/*
 * Map a request URI to a file path under `root`: strip query/fragment,
 * percent-decode, then normalize segments, rejecting NUL bytes and any
 * ".." that would escape the root (returns false).
 */
bool uri_to_safe_path(const std::string& root, const char *uri,
					  std::string *out);

} // namespace ferry

#endif // FERRY_HTTP_PATH_H
