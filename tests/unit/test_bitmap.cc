#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>
#include <gtest/gtest.h>
#include "bitmap.h"

namespace
{

/* Unique /tmp path per process; removed on destruction. */
class TmpMetaPath
{
public:
	explicit TmpMetaPath(const std::string& tag)
	{
		static int counter = 0;
		path_ = "/tmp/ferry_bitmap_test_" + std::to_string(getpid()) + "_" +
				tag + "_" + std::to_string(counter++) + ".json";
	}

	~TmpMetaPath() { remove(path_.c_str()); }

	const std::string& path() const { return path_; }

private:
	std::string path_;
};

ferry::DownloadMeta sample_meta()
{
	ferry::DownloadMeta meta;
	meta.url = "http://host:8080/dir/big.bin";
	meta.size = 123456789;
	meta.last_modified = "Tue, 15 Nov 1994 12:45:26 GMT";
	meta.chunk_size = 8LL * 1024 * 1024;
	meta.bitmap.reset(15);
	meta.bitmap.mark(0);
	meta.bitmap.mark(7);
	meta.bitmap.mark(14);
	return meta;
}

/* ---------- bitmap ---------- */

TEST(Bitmap, MarkTestCountDone)
{
	ferry::ChunkBitmap bm(10);
	EXPECT_EQ(bm.count(), 10);
	EXPECT_EQ(bm.done(), 0);
	EXPECT_FALSE(bm.test(0));
	EXPECT_FALSE(bm.test(9));

	bm.mark(0);
	bm.mark(3);
	bm.mark(9);
	EXPECT_TRUE(bm.test(0));
	EXPECT_FALSE(bm.test(1));
	EXPECT_FALSE(bm.test(2));
	EXPECT_TRUE(bm.test(3));
	EXPECT_TRUE(bm.test(9));
	EXPECT_EQ(bm.done(), 3);

	bm.mark(3);					/* idempotent */
	EXPECT_EQ(bm.done(), 3);
}

TEST(Bitmap, OutOfRangeMarkIgnored)
{
	ferry::ChunkBitmap bm(5);
	bm.mark(-1);
	bm.mark(5);
	bm.mark(1000000);
	EXPECT_EQ(bm.count(), 5);
	EXPECT_EQ(bm.done(), 0);
	EXPECT_FALSE(bm.test(-1));
	EXPECT_FALSE(bm.test(5));
	EXPECT_FALSE(bm.test(1000000));
}

TEST(Bitmap, ResetClearsAndResizes)
{
	ferry::ChunkBitmap bm(8);
	bm.mark(2);
	bm.mark(7);
	EXPECT_EQ(bm.done(), 2);

	bm.reset(3);
	EXPECT_EQ(bm.count(), 3);
	EXPECT_EQ(bm.done(), 0);
	EXPECT_FALSE(bm.test(2));
	EXPECT_FALSE(bm.test(7));
}

TEST(Bitmap, EmptyBitmap)
{
	ferry::ChunkBitmap bm;
	EXPECT_EQ(bm.count(), 0);
	EXPECT_EQ(bm.done(), 0);
	EXPECT_EQ(bm.serialize(), "");

	ferry::ChunkBitmap restored;
	EXPECT_TRUE(restored.deserialize("", 0));
	EXPECT_EQ(restored.count(), 0);
}

TEST(Bitmap, SerializeDeserializeRoundTrip)
{
	ferry::ChunkBitmap bm(13);
	bm.mark(0);
	bm.mark(5);
	bm.mark(12);

	std::string hex = bm.serialize();
	/* 13 chunks => 2 bytes; byte0: bits 0+5 = 0x21, byte1: bit 4 = 0x10 */
	EXPECT_EQ(hex, "2110");

	ferry::ChunkBitmap restored;
	ASSERT_TRUE(restored.deserialize(hex, 13));
	EXPECT_EQ(restored.count(), 13);
	EXPECT_EQ(restored.done(), 3);
	for (long long i = 0; i < 13; i++)
		EXPECT_EQ(restored.test(i), bm.test(i)) << "chunk " << i;
}

TEST(Bitmap, DeserializeRejectsWrongCount)
{
	ferry::ChunkBitmap bm(13);
	bm.mark(0);
	bm.mark(5);
	bm.mark(12);
	std::string hex = bm.serialize();		/* "2110" */

	ferry::ChunkBitmap out(13);
	out.mark(1);							/* sentinel state */

	EXPECT_FALSE(out.deserialize(hex, 8));	/* different byte count */
	EXPECT_FALSE(out.deserialize(hex, 17));
	EXPECT_FALSE(out.deserialize(hex, 12));	/* bit 12 would be out of range */
	EXPECT_FALSE(out.deserialize(hex, -1));

	/* failed attempts must leave the bitmap untouched */
	EXPECT_EQ(out.count(), 13);
	EXPECT_EQ(out.done(), 1);
	EXPECT_TRUE(out.test(1));
}

TEST(Bitmap, DeserializeRejectsNonHex)
{
	ferry::ChunkBitmap out;
	EXPECT_FALSE(out.deserialize("zz", 8));
	EXPECT_FALSE(out.deserialize("1g", 8));
	EXPECT_FALSE(out.deserialize("abc", 8));	/* odd byte coverage */
	EXPECT_FALSE(out.deserialize("211", 13));	/* short */
	EXPECT_FALSE(out.deserialize("21100", 13));	/* long */
	EXPECT_FALSE(out.deserialize("2190", 13));	/* stray bit past count */
}

/* ---------- meta JSON ---------- */

TEST(Meta, JsonRoundTripWithAwkwardStrings)
{
	ferry::DownloadMeta meta;
	meta.url = "http://example.com/a \"quoted\" \\dir\\file.bin?q=1&r=2";
	meta.size = 8388609;
	meta.last_modified = "Wed, 21 Oct 2025 07:28:00 GMT";
	meta.chunk_size = 8LL * 1024 * 1024;
	meta.bitmap.reset(2);
	meta.bitmap.mark(1);

	std::string json = ferry::meta_to_json(meta);

	ferry::DownloadMeta parsed;
	ASSERT_TRUE(ferry::meta_from_json(json, &parsed));
	EXPECT_EQ(parsed.version, 1);
	EXPECT_EQ(parsed.url, meta.url);
	EXPECT_EQ(parsed.size, meta.size);
	EXPECT_EQ(parsed.last_modified, meta.last_modified);
	EXPECT_EQ(parsed.chunk_size, meta.chunk_size);
	EXPECT_EQ(parsed.bitmap.count(), 2);
	EXPECT_EQ(parsed.bitmap.done(), 1);
	EXPECT_FALSE(parsed.bitmap.test(0));
	EXPECT_TRUE(parsed.bitmap.test(1));
}

TEST(Meta, JsonContainsRequiredFields)
{
	ferry::DownloadMeta meta = sample_meta();
	std::string json = ferry::meta_to_json(meta);

	EXPECT_NE(json.find("\"version\":1"), std::string::npos);
	EXPECT_NE(json.find("\"url\":"), std::string::npos);
	EXPECT_NE(json.find("\"size\":123456789"), std::string::npos);
	EXPECT_NE(json.find("\"last_modified\":"), std::string::npos);
	EXPECT_NE(json.find("\"chunk_size\":"), std::string::npos);
	EXPECT_NE(json.find("\"chunk_count\":15"), std::string::npos);
	EXPECT_NE(json.find("\"bitmap\":"), std::string::npos);
}

TEST(Meta, CorruptJsonRejected)
{
	ferry::DownloadMeta meta = sample_meta();
	std::string json = ferry::meta_to_json(meta);

	ferry::DownloadMeta out;
	EXPECT_FALSE(ferry::meta_from_json("", &out));
	EXPECT_FALSE(ferry::meta_from_json("{", &out));
	EXPECT_FALSE(ferry::meta_from_json("null", &out));
	EXPECT_FALSE(ferry::meta_from_json("[1, 2, 3]", &out));
	EXPECT_FALSE(ferry::meta_from_json(json.substr(0, json.size() / 2), &out));
	EXPECT_FALSE(ferry::meta_from_json(json + "garbage", &out));
}

TEST(Meta, WrongVersionRejected)
{
	ferry::DownloadMeta meta = sample_meta();
	meta.version = 2;
	std::string json = ferry::meta_to_json(meta);

	ferry::DownloadMeta out;
	EXPECT_FALSE(ferry::meta_from_json(json, &out));
}

TEST(Meta, MissingFieldRejected)
{
	/* valid meta JSON minus the last_modified field */
	std::string json =
		"{\"version\":1,\"url\":\"http://x/y\",\"size\":10,"
		"\"chunk_size\":5,\"chunk_count\":2,\"bitmap\":\"01\"}";

	ferry::DownloadMeta out;
	EXPECT_FALSE(ferry::meta_from_json(json, &out));
}

TEST(Meta, ExtraFieldRejected)
{
	ferry::DownloadMeta meta = sample_meta();
	std::string json = ferry::meta_to_json(meta);
	/* splice an unknown key in after the opening brace */
	json.insert(1, "\"extra\":0,");

	ferry::DownloadMeta out;
	EXPECT_FALSE(ferry::meta_from_json(json, &out));
}

TEST(Meta, DuplicatedFieldRejected)
{
	ferry::DownloadMeta meta = sample_meta();
	std::string json = ferry::meta_to_json(meta);
	json.insert(1, "\"size\":5,");

	ferry::DownloadMeta out;
	EXPECT_FALSE(ferry::meta_from_json(json, &out));
}

TEST(Meta, WrongTypedFieldRejected)
{
	/* size given as a JSON string instead of a number */
	std::string json =
		"{\"version\":1,\"url\":\"http://x/y\",\"size\":\"10\","
		"\"last_modified\":\"now\",\"chunk_size\":5,"
		"\"chunk_count\":2,\"bitmap\":\"01\"}";

	ferry::DownloadMeta out;
	EXPECT_FALSE(ferry::meta_from_json(json, &out));

	/* url given as a number instead of a string */
	json =
		"{\"version\":1,\"url\":7,\"size\":10,"
		"\"last_modified\":\"now\",\"chunk_size\":5,"
		"\"chunk_count\":2,\"bitmap\":\"01\"}";
	EXPECT_FALSE(ferry::meta_from_json(json, &out));

	/* bitmap hex that does not cover chunk_count bytes */
	json =
		"{\"version\":1,\"url\":\"http://x/y\",\"size\":10,"
		"\"last_modified\":\"now\",\"chunk_size\":5,"
		"\"chunk_count\":2,\"bitmap\":\"0102\"}";
	EXPECT_FALSE(ferry::meta_from_json(json, &out));
}

/* ---------- persistence ---------- */

TEST(Persistence, AtomicSaveLoadRoundTrip)
{
	TmpMetaPath f("round");
	ferry::DownloadMeta meta = sample_meta();
	ASSERT_TRUE(ferry::save_meta_atomic(f.path(), meta));

	/* the temp file must be gone after the rename */
	std::string tmp = f.path() + ".tmp." + std::to_string(getpid());
	EXPECT_EQ(access(tmp.c_str(), F_OK), -1);

	ferry::DownloadMeta loaded;
	ASSERT_TRUE(ferry::load_meta(f.path(), &loaded));
	EXPECT_EQ(loaded.version, meta.version);
	EXPECT_EQ(loaded.url, meta.url);
	EXPECT_EQ(loaded.size, meta.size);
	EXPECT_EQ(loaded.last_modified, meta.last_modified);
	EXPECT_EQ(loaded.chunk_size, meta.chunk_size);
	EXPECT_EQ(loaded.bitmap.count(), meta.bitmap.count());
	EXPECT_EQ(loaded.bitmap.done(), meta.bitmap.done());
	for (long long i = 0; i < meta.bitmap.count(); i++)
		EXPECT_EQ(loaded.bitmap.test(i), meta.bitmap.test(i));

	/* save again over the existing file: atomic replace keeps working */
	ferry::DownloadMeta meta2 = meta;
	meta2.bitmap.mark(3);
	meta2.size += 1;
	ASSERT_TRUE(ferry::save_meta_atomic(f.path(), meta2));

	ferry::DownloadMeta reloaded;
	ASSERT_TRUE(ferry::load_meta(f.path(), &reloaded));
	EXPECT_EQ(reloaded.size, meta2.size);
	EXPECT_TRUE(reloaded.bitmap.test(3));
	EXPECT_EQ(reloaded.bitmap.done(), 4);
}

TEST(Persistence, LoadMissingFileFails)
{
	std::string path = "/tmp/ferry_bitmap_missing_" +
			std::to_string(getpid()) + ".json";
	ferry::DownloadMeta out;
	EXPECT_FALSE(ferry::load_meta(path, &out));
}

TEST(Persistence, CorruptFileOnDiskFails)
{
	TmpMetaPath f("corrupt");
	{
		std::ofstream out(f.path());
		out << "this is not meta json";
	}
	ferry::DownloadMeta out;
	EXPECT_FALSE(ferry::load_meta(f.path(), &out));

	/* a truncated formerly-valid file is corrupt too */
	ferry::DownloadMeta meta = sample_meta();
	std::string json = ferry::meta_to_json(meta);
	{
		std::ofstream out(f.path());
		out << json.substr(0, json.size() - 5);
	}
	EXPECT_FALSE(ferry::load_meta(f.path(), &out));
}

TEST(Persistence, SaveToUnwritablePathFails)
{
	ferry::DownloadMeta meta = sample_meta();
	std::string path = "/ferry_no_such_dir_" + std::to_string(getpid()) +
			"/meta.json";
	EXPECT_FALSE(ferry::save_meta_atomic(path, meta));
}

} // namespace
