// Both kernels' boot setup lets IconServices' rendered icons go once per GPU build the deploy names
// (tools/deploy/icon_cache.py), before launchd starts iconservicesagent, without setting up the cache's directories.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {
const char *const kKernels[] = {"../xnu-10063.141.1/bsd/kern/kern_exec.c", "../xnu-12377.121.6/bsd/kern/kern_exec.c"};
constexpr const char *kIconCache = "native_switch_icon_cache(proc_t p, user_addr_t scratch, mount_t data_mount)";
constexpr const char *kUnlinkAll = "native_switch_icon_cache_unlink_all(proc_t p, user_addr_t scratch, int *dropped)";
constexpr const char *kUnlink =
    "native_switch_icon_cache_unlink(proc_t p, user_addr_t scratch, const char *salt, int *dropped)";
constexpr const char *kSetup = "native_switch_setup_state(proc_t p, user_addr_t scratch, mount_t data_mount)";
constexpr const char *kUnchanged = "native_switch_file_unchanged(marker, gpu, length, data_mount, 0, 0, 0644)";
constexpr const char *kDrop = "native_switch_icon_cache_unlink_all(p, scratch, &dropped)";
constexpr const char *kMark = "native_switch_watchdog_file(p, scratch, marker, gpu, length, data_mount, 0, 0, 0644)";
}  // namespace

TEST(XnuIconCacheSource, TheKernelReadsTheRecordTheDeployWrites)
{
    const std::string record = Between(SourceText("../../tools/deploy/icon_cache.py"), "\nRECORD = '", "'");
    ASSERT_FALSE(record.empty());
    for (const char *path : kKernels) {
        const std::string body = Body(SourceText(path), kIconCache);
        EXPECT_THAT(body, HasSubstr("request[] = \"/private/var/Switch/" + record + "\"")) << path;
    }
}

// The marker is compared before anything goes and written after, so the same GPU build keeps the icons and a boot cut
// short drops them again; only the marker is written, and no directory is made or given an owner (the agent resets a
// cache directory that is not _iconservices').
TEST(XnuIconCacheSource, EachCacheInfoGoesOncePerGpuBuild)
{
    std::string first;
    for (const char *path : kKernels) {
        const std::string source = SourceText(path);
        const std::string body = Body(source, kIconCache), all = Body(source, kUnlinkAll), one = Body(source, kUnlink);
        ASSERT_FALSE(body.empty() || all.empty() || one.empty()) << path;
        EXPECT_THAT(source, HasSubstr("\"systemgroup.com.apple.lsd.iconscache/Library/Caches/com.apple.IconsCache\""))
            << path;
        EXPECT_THAT(one, HasSubstr("__cache_info__\"")) << path;
        const size_t unchanged = body.find(kUnchanged), drop = body.find(kDrop), mark = body.find(kMark);
        ASSERT_NE(unchanged, std::string::npos) << path;
        EXPECT_LT(unchanged, drop) << path;
        EXPECT_LT(drop, mark) << path;
        EXPECT_EQ(Count(body + all + one, "native_switch_watchdog_file("), 1u) << path;
        EXPECT_EQ(Count(body + all + one, "native_switch_mkdir("), 0u) << path;
        EXPECT_EQ(Count(body + all + one, "native_switch_watchdog_directory("), 0u) << path;
        EXPECT_EQ(Count(Body(source, kSetup), "native_switch_icon_cache(p, scratch, data_mount);"), 1u) << path;
        if (first.empty())
            first = body + all + one;
        EXPECT_EQ(body + all + one, first) << path << ": the two kernels' drops differ";
    }
}

// 21H461 keeps __cache_info__ at the cache's top, 24B119 in a salt folder (-[ISDefaults cacheURL]): both go.
TEST(XnuIconCacheSource, TheTopAndEverySaltFolderLoseTheirCacheInfo)
{
    for (const char *path : kKernels) {
        const std::string all = Body(SourceText(path), kUnlinkAll);
        const size_t top = all.find("native_switch_icon_cache_unlink(p, scratch, NULL, dropped)");
        const size_t walk = all.find("VNOP_READDIR(cache, uio, 0, &eof, &count, context)");
        ASSERT_NE(top, std::string::npos) << path;
        EXPECT_LT(top, walk) << path;
        EXPECT_THAT(all, HasSubstr("entry->d_type == DT_DIR && entry->d_name[0] != '.'")) << path;
        EXPECT_THAT(all, HasSubstr("native_switch_icon_cache_unlink(p, scratch, entry->d_name, dropped)")) << path;
    }
}
