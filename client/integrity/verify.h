#ifndef FERRY_VERIFY_H
#define FERRY_VERIFY_H

#include <string>

namespace ferry
{

/*
 * Compute the sha256 of the file at `path` by chaining pread tasks of
 * `chunk_bytes` each into one series and feeding EVP_DigestUpdate.
 * Blocks the calling thread (WFFacilities::WaitGroup).
 * Returns the digest as lowercase hex, or "" on any error (missing file,
 * IO failure).
 */
std::string sha256_of_file(const std::string& path,
						   long long chunk_bytes = 8LL * 1024 * 1024);

/*
 * `spec` is "sha-256=<hex>" (case-insensitive prefix and hex).
 * Returns false for any other scheme or a malformed spec.
 */
bool checksum_spec_matches(const std::string& spec,
							const std::string& digest_hex);

} // namespace ferry

#endif // FERRY_VERIFY_H
