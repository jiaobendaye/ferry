#include <map>
#include <string>
#include <gtest/gtest.h>

#include "observability/cgroup_memory.h"

namespace
{

TEST(CgroupMemory, ResolvesRootAndNonstandardMounts)
{
	std::string path;
	EXPECT_TRUE(ferry::CgroupMemoryReader::resolve_memory_stat_path(
		"0::/user.slice/app.scope\n",
		"24 20 0:22 / /sys/fs/cgroup rw - cgroup2 cgroup rw\n", &path));
	EXPECT_EQ(path, "/sys/fs/cgroup/user.slice/app.scope/memory.stat");

	EXPECT_TRUE(ferry::CgroupMemoryReader::resolve_memory_stat_path(
		"0::/user.slice/my\\040service\n",
		"24 20 0:22 /user.slice /custom\\040cg rw - cgroup2 cgroup rw\n",
		&path));
	EXPECT_EQ(path, "/custom cg/my service/memory.stat");
}

TEST(CgroupMemory, RejectsMissingUnifiedHierarchyOrMismatchedRoot)
{
	std::string path;
	EXPECT_FALSE(ferry::CgroupMemoryReader::resolve_memory_stat_path(
		"2:cpu:/legacy\n", "24 20 0:22 / /cg rw - cgroup2 cgroup rw\n",
		&path));
	EXPECT_FALSE(ferry::CgroupMemoryReader::resolve_memory_stat_path(
		"0::/other/scope\n",
		"24 20 0:22 /user.slice /cg rw - cgroup2 cgroup rw\n", &path));
}

TEST(CgroupMemory, ParsesFieldsIndependently)
{
	auto full = ferry::CgroupMemoryReader::parse_memory_stat(
		"anon 123\nfile 456\nsock 789\ninactive_file 10\n");
	EXPECT_EQ(full.anon, 123);
	EXPECT_EQ(full.file, 456);
	EXPECT_EQ(full.sock, 789);

	auto partial = ferry::CgroupMemoryReader::parse_memory_stat(
		"anon broken\nfile 0\nother 1\n");
	EXPECT_EQ(partial.anon, -1);
	EXPECT_EQ(partial.file, 0);
	EXPECT_EQ(partial.sock, -1);
}

TEST(CgroupMemory, InjectedReaderSamplesResolvedMemoryStat)
{
	std::map<std::string, std::string> files = {
		{"/proc/self/cgroup", "0::/demo\n"},
		{"/proc/self/mountinfo",
		 "24 20 0:22 / /sys/fs/cgroup rw - cgroup2 cgroup rw\n"},
		{"/sys/fs/cgroup/demo/memory.stat", "anon 11\nfile 22\nsock 33\n"},
	};
	ferry::CgroupMemoryReader reader(
		[&files](const std::string& path, std::string *out) {
			auto it = files.find(path);
			if (it == files.end())
				return false;
			*out = it->second;
			return true;
		});
	auto snapshot = reader.snapshot();
	EXPECT_EQ(snapshot.anon, 11);
	EXPECT_EQ(snapshot.file, 22);
	EXPECT_EQ(snapshot.sock, 33);
}

TEST(CgroupMemory, ReadFailureReturnsUnavailable)
{
	ferry::CgroupMemoryReader reader(
		[](const std::string&, std::string *) { return false; });
	auto snapshot = reader.snapshot();
	EXPECT_EQ(snapshot.anon, -1);
	EXPECT_EQ(snapshot.file, -1);
	EXPECT_EQ(snapshot.sock, -1);
}

} // namespace
