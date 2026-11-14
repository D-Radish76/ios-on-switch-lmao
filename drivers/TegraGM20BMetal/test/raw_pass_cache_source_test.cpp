// SPDX-License-Identifier: MIT
// The pass cache's path on the encoding thread (RawMetalPassCache.mm rmPassFind, rmPassDone, rmPassKeep and what they
// call) takes no lock and makes no atomic read-modify-write: only rmPassTrim and the thread's first and last use touch
// the registry's mutex. rmPassTrim and the thread meet through two flags each stores before it loads the other's
// (sequentially consistent), and the trim takes a thread's copies only where it saw that thread outside its window.
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

namespace {

const std::string &cache()
{
    static const std::string source = SourceText("../raw/runtime/RawMetalPassCache.mm");
    return source;
}

TEST(RawPassCacheSource, ThePassPathTakesNoLockAndNoReadModifyWrite)
{
    for (const char *head :
         {"RMPassEntry *rmPassFind(", "void rmPassDone()", "void rmPassKeep(", "bool enter(Cache &c)",
          "void leave(Cache &c)", "void dropIdle(Cache &c,", "CFTypeRef take(Cache &c, size_t i)",
          "void count(Cache &c, int64_t passes, int64_t textures)"}) {
        const CodeText body = Block(cache(), head);
        for (const char *banned :
             {"_lock", "lock_guard", "mutex", "fetch_", "exchange", "__atomic", "OSAtomic", "@synchronized"})
            EXPECT_FALSE(body.has(banned)) << head << " " << banned;
    }
}

TEST(RawPassCacheSource, TrimTakesCopiesOnlyWhereItSawTheThreadOutsideItsWindow)
{
    const CodeText trim = Block(cache(), "RMPassTrim rmPassTrim()");
    EXPECT_FALSE(trim.has("thread_suspend")) << "a stopped thread's stores reach the trim through no edge TSAN sees";
    EXPECT_TRUE(trim.before("c->trimming.store(true, std::memory_order_seq_cst);",
                            "if (c->inside.load(std::memory_order_seq_cst))"));
    EXPECT_TRUE(trim.before("if (c->inside.load(std::memory_order_seq_cst))", "gone[i] = take(*c, i);"));
    EXPECT_TRUE(trim.before("gone[i] = take(*c, i);", "c->trimming.store(false, std::memory_order_release);"));
    const CodeText enter = Block(cache(), "bool enter(Cache &c)");
    EXPECT_TRUE(enter.before("c.inside.store(true, std::memory_order_seq_cst);",
                             "if (!c.trimming.load(std::memory_order_seq_cst))"));
    for (const char *path : {"RMPassEntry *rmPassFind(", "void rmPassKeep("})
        EXPECT_TRUE(Block(cache(), path).has("if (!enter(c))")) << path << " goes without the cache a trim holds";
}

}  // namespace
