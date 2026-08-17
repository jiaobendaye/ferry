#ifndef FERRY_BITMAP_H
#define FERRY_BITMAP_H

#include <string>
#include <vector>

namespace ferry
{

/*
 * Completion bitmap for the chunked download: bit i set <=> chunk i is
 * fully written. Bit layout is stable across versions and is what gets
 * persisted in the meta file (see serialize()/deserialize()).
 */
class ChunkBitmap
{
public:
	ChunkBitmap() = default;
	explicit ChunkBitmap(long long count);

	void reset(long long count);		/* all incomplete */
	void mark(long long idx);			/* out-of-range: ignored */
	bool test(long long idx) const;		/* out-of-range: false */
	long long count() const;
	long long done() const;

	/* Packed bits as a lowercase hex string (2 chars per byte). */
	std::string serialize() const;
	/*
	 * Inverse of serialize(). Fails on bad hex, on a hex length that does
	 * not match `count`, or on stray bits beyond the last chunk; the
	 * bitmap is left unchanged on failure.
	 */
	bool deserialize(const std::string& hex, long long count);

private:
	long long count_ = 0;
	std::vector<unsigned char> bits_;	/* bit idx: byte idx/8, bit idx%8 */
};

/*
 * Resume state for one download, persisted as <output>.ferry.json.
 * last_modified is the raw HTTP date string, compared verbatim against
 * the server's HEAD response on restart.
 */
struct DownloadMeta
{
	std::string url;
	long long size = 0;
	std::string last_modified;
	long long chunk_size = 0;
	int version = 1;
	ChunkBitmap bitmap;
};

/* Serialize meta to JSON text (hand-rolled; no external library). */
std::string meta_to_json(const DownloadMeta& meta);
/* Parse JSON produced by meta_to_json(). False on any parse problem or
 * version != 1; *out is untouched on failure. */
bool meta_from_json(const std::string& json, DownloadMeta *out);
/* Write JSON to path atomically: <path>.tmp.<pid> then rename() over
 * path. False on IO error. */
bool save_meta_atomic(const std::string& path, const DownloadMeta& meta);
/* Read + parse. False when the file is missing, unreadable or corrupt. */
bool load_meta(const std::string& path, DownloadMeta *out);

} // namespace ferry

#endif
