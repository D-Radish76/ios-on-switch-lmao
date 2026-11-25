// The D0 loader's decisions (checks.c) and its screen text (report.c), on the host: each precondition passes on the
// reviewed values and fails with its own code on every way of being wrong.
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

extern "C" {
#include "../checks.h"
#include "../mc_table.h"
#include "../report.h"
}

namespace {
const switch_diag_span IMAGE = {0xa0000000, 0xa0015000};
const switch_diag_span FB = {SWITCH_DIAG_FB_PA, SWITCH_DIAG_FB_PA + SWITCH_DIAG_FB_BYTES};
constexpr uint64_t A57 = 0x410fd070;

std::vector<uint32_t> table(std::initializer_list<std::pair<uint64_t, uint64_t>> entries, bool terminate = true)
{
    std::vector<uint32_t> words;
    auto put = [&](uint64_t v) {
        words.push_back(__builtin_bswap32(static_cast<uint32_t>(v >> 32)));
        words.push_back(__builtin_bswap32(static_cast<uint32_t>(v)));
    };
    for (auto [base, size] : entries) {
        put(base);
        put(size);
    }
    if (terminate) {
        put(0);
        put(0);
    }
    return words;
}

int reservations(const std::vector<uint32_t> &words, uint32_t *entries, int *covers)
{
    return switch_diag_fdt_reservations(words.data(), 0, static_cast<uint32_t>(words.size() * 4), FB, entries, covers);
}
}  // namespace

TEST(DiagEntry, TheReviewedStatePasses)
{
    EXPECT_EQ(0, switch_diag_entry_state(8, 0, 0, 0, 0, A57, 19200000));
}

TEST(DiagEntry, EachDeviationHasItsOwnCode)
{
    EXPECT_EQ(SWITCH_DIAG_ENTRY_EL, switch_diag_entry_state(4, 0, 0, 0, 0, A57, 19200000));
    EXPECT_EQ(SWITCH_DIAG_ENTRY_EL, switch_diag_entry_state(8, 1, 0, 0, 0, A57, 19200000));
    EXPECT_EQ(SWITCH_DIAG_ENTRY_EL, switch_diag_entry_state(8, 4, 0, 0, 0, A57, 19200000));
    EXPECT_EQ(SWITCH_DIAG_ENTRY_EL, switch_diag_entry_state(8, 0x1000, 0, 0, 0, A57, 19200000));
    EXPECT_EQ(SWITCH_DIAG_ENTRY_ARGUMENTS, switch_diag_entry_state(8, 0, 1, 0, 0, A57, 19200000));
    EXPECT_EQ(SWITCH_DIAG_ENTRY_ARGUMENTS, switch_diag_entry_state(8, 0, 0, 0, 7, A57, 19200000));
    EXPECT_EQ(SWITCH_DIAG_ENTRY_CPU, switch_diag_entry_state(8, 0, 0, 0, 0, 0x411fd080, 19200000));
    EXPECT_EQ(SWITCH_DIAG_ENTRY_CPU, switch_diag_entry_state(8, 0, 0, 0, 0, 0x510fd070, 19200000));
    EXPECT_EQ(SWITCH_DIAG_ENTRY_TIMER, switch_diag_entry_state(8, 0, 0, 0, 0, A57, 38400000));
    EXPECT_EQ(SWITCH_DIAG_ENTRY_TIMER, switch_diag_entry_state(8, 0, 0, 0, 0, A57, 0));
}

TEST(DiagEl1, TheStateTheNormalLoaderRequires)
{
    const switch_diag_el1 good = {4, 0x30d00800, 0x3c0, 19200000, 0x80000000, 3, 0};
    EXPECT_EQ(0, switch_diag_el1_state(&good));
    for (int field = 0; field < 7; ++field) {
        switch_diag_el1 bad = good;
        uint64_t *words[] = {&bad.current_el, &bad.sctlr, &bad.daif, &bad.cntfrq, &bad.hcr, &bad.cnthctl, &bad.cntvoff};
        *words[field] ^= field == 1 ? 1 : field == 2 ? 0x80 : 0x10;
        EXPECT_EQ(SWITCH_DIAG_EL1_STATE, switch_diag_el1_state(&bad)) << field;
    }
}

TEST(DiagFdt, OnlyTheScriptsPageInsideDramAndClearOfTheImage)
{
    EXPECT_EQ(0, switch_diag_fdt_pointer(0x8d000000, IMAGE));
    EXPECT_EQ(SWITCH_DIAG_FDT_ADDRESS, switch_diag_fdt_pointer(0x8d000008, IMAGE));
    EXPECT_EQ(SWITCH_DIAG_FDT_ADDRESS, switch_diag_fdt_pointer(0, IMAGE));
    EXPECT_EQ(SWITCH_DIAG_FDT_OVERLAPS_IMAGE, switch_diag_fdt_pointer(0x8d000000, {0x8d010000, 0x8d020000}));
    EXPECT_EQ(0, switch_diag_fdt_pointer(0x8d000000, {0x8d040000, 0x8d050000}));
}

TEST(DiagFdt, HeaderWords)
{
    EXPECT_EQ(0, switch_diag_fdt_header(0xd00dfeed, 168190, 0x28, 0x48));
    EXPECT_EQ(SWITCH_DIAG_FDT_MAGIC, switch_diag_fdt_header(0xedfe0dd0, 168190, 0x28, 0x48));
    EXPECT_EQ(SWITCH_DIAG_FDT_TOTAL, switch_diag_fdt_header(0xd00dfeed, 39, 0x28, 0x48));
    EXPECT_EQ(SWITCH_DIAG_FDT_TOTAL, switch_diag_fdt_header(0xd00dfeed, 0x40001, 0x28, 0x48));
    EXPECT_EQ(0, switch_diag_fdt_header(0xd00dfeed, 0x40000, 0x28, 0x48));
    EXPECT_EQ(SWITCH_DIAG_FDT_OFFSETS, switch_diag_fdt_header(0xd00dfeed, 168190, 0x20, 0x48));
    EXPECT_EQ(SWITCH_DIAG_FDT_OFFSETS, switch_diag_fdt_header(0xd00dfeed, 168190, 0x2c, 0x48));
    EXPECT_EQ(SWITCH_DIAG_FDT_OFFSETS, switch_diag_fdt_header(0xd00dfeed, 168190, 0x28, 0x4a));
    EXPECT_EQ(SWITCH_DIAG_FDT_OFFSETS, switch_diag_fdt_header(0xd00dfeed, 168190, 0x28, 0x30));
    EXPECT_EQ(SWITCH_DIAG_FDT_OFFSETS, switch_diag_fdt_header(0xd00dfeed, 100, 0x28, 0x68));
}

TEST(DiagFdt, ReservationsAreCountedAndTheFramebufferMustBeInsideOne)
{
    uint32_t entries;
    int covers;
    EXPECT_EQ(0, reservations(table({{0x80000000, 0x20000}, {0xf5a00000, 0x400000}}), &entries, &covers));
    EXPECT_EQ(2u, entries);
    EXPECT_TRUE(covers);
    EXPECT_EQ(0, reservations(table({{0x80000000, 0x20000}}), &entries, &covers));
    EXPECT_FALSE(covers);
    EXPECT_EQ(0, reservations(table({{0xf5a00000, 0x383000}}), &entries, &covers));
    EXPECT_FALSE(covers) << "a reservation that stops short of the last pixel does not cover the framebuffer";
    EXPECT_EQ(0, reservations(table({{0xf5a00000, 0x384000}}), &entries, &covers));
    EXPECT_TRUE(covers);
    EXPECT_EQ(0, reservations(table({{0xf5a01000, 0x400000}}), &entries, &covers));
    EXPECT_FALSE(covers);
}

TEST(DiagFdt, ABrokenReservationBlockStopsTheReadWhereItIs)
{
    uint32_t entries;
    int covers;
    EXPECT_EQ(SWITCH_DIAG_FDT_RESERVATION_END, reservations(table({{0x80000000, 0x20000}}, false), &entries, &covers));
    EXPECT_EQ(SWITCH_DIAG_FDT_RESERVATION_SIZE, reservations(table({{0x80000000, 0}}), &entries, &covers));
    EXPECT_EQ(SWITCH_DIAG_FDT_RESERVATION_SIZE, reservations(table({{UINT64_MAX - 4, 0x20}}), &entries, &covers));
    std::vector<std::pair<uint64_t, uint64_t>> many;
    for (unsigned i = 0; i < SWITCH_DIAG_RESERVATIONS_MAX + 1; ++i)
        many.push_back({0x80000000 + i * 0x1000, 0x1000});
    std::vector<uint32_t> words;
    for (auto [base, size] : many) {
        auto one = table({{base, size}}, false);
        words.insert(words.end(), one.begin(), one.end());
    }
    auto end = table({}, true);
    words.insert(words.end(), end.begin(), end.end());
    EXPECT_EQ(SWITCH_DIAG_FDT_RESERVATION_COUNT, reservations(words, &entries, &covers));
}

TEST(DiagFramebuffer, TheReviewedWindowIsDrawableOnlyWhenReservedAndClearOfTheImage)
{
    EXPECT_EQ(0, switch_diag_framebuffer(SWITCH_DIAG_FB_PA, SWITCH_DIAG_FB_BYTES, 1, IMAGE));
    EXPECT_EQ(SWITCH_DIAG_FB_NOT_RESERVED, switch_diag_framebuffer(SWITCH_DIAG_FB_PA, SWITCH_DIAG_FB_BYTES, 0, IMAGE));
    EXPECT_EQ(SWITCH_DIAG_FB_GEOMETRY, switch_diag_framebuffer(SWITCH_DIAG_FB_PA, 0, 1, IMAGE));
    EXPECT_EQ(SWITCH_DIAG_FB_GEOMETRY, switch_diag_framebuffer(SWITCH_DIAG_FB_PA + 4, 0x1000, 1, IMAGE));
    EXPECT_EQ(SWITCH_DIAG_FB_GEOMETRY, switch_diag_framebuffer(SWITCH_DIAG_FB_PA, UINT64_MAX, 1, IMAGE));
    EXPECT_EQ(SWITCH_DIAG_FB_DRAM, switch_diag_framebuffer(0x7ffff000, 0x2000, 1, IMAGE));
    EXPECT_EQ(SWITCH_DIAG_FB_DRAM, switch_diag_framebuffer(0xffcff000, 0x2000, 1, IMAGE));
    EXPECT_EQ(0, switch_diag_framebuffer(0xffcfe000, 0x2000, 1, IMAGE));
    EXPECT_EQ(SWITCH_DIAG_FB_OVERLAPS_IMAGE, switch_diag_framebuffer(0xa0014000, 0x2000, 1, IMAGE));
    EXPECT_EQ(0, switch_diag_framebuffer(0xa0015000, 0x2000, 1, IMAGE));
}

TEST(DiagTimer, TheWaitIsFifteenSecondsOfTheNineteenPointTwoMegahertzCounter)
{
    EXPECT_EQ(UINT64_C(288000000), switch_diag_wait_ticks(19200000, SWITCH_DIAG_WAIT_SECONDS));
    EXPECT_EQ(0u, switch_diag_wait_ticks(0, 15));
    EXPECT_EQ(0u, switch_diag_wait_ticks(38400000, 15));
    EXPECT_EQ(0u, switch_diag_wait_ticks(19200001, 15));
    EXPECT_EQ(0u, switch_diag_wait_ticks(19200000, 0));
    EXPECT_EQ(0u, switch_diag_wait_ticks(19200000, 61));
}

TEST(DiagTimer, TheDeadlineSurvivesTheCounterWrapping)
{
    EXPECT_TRUE(switch_diag_before(100, 200));
    EXPECT_FALSE(switch_diag_before(200, 200));
    EXPECT_FALSE(switch_diag_before(300, 200));
    EXPECT_TRUE(switch_diag_before(UINT64_MAX - 5, 10));
    EXPECT_FALSE(switch_diag_before(11, 10));
}

TEST(DiagReport, HexAndDecimalAreFixedWidthAndExact)
{
    switch_diag_line line;
    switch_diag_line_start(&line, "X ");
    switch_diag_line_hex(&line, 0x2117, 8);
    switch_diag_line_add(&line, " ");
    switch_diag_line_decimal(&line, 0);
    switch_diag_line_add(&line, " ");
    switch_diag_line_decimal(&line, 19200000);
    EXPECT_STREQ("X 00002117 0 19200000", line.text);
    switch_diag_line_start(&line, "");
    switch_diag_line_decimal(&line, UINT64_MAX);
    EXPECT_STREQ("18446744073709551615", line.text);
}

TEST(DiagReport, ARegisterLineSaysSameOrShowsBothValues)
{
    switch_diag_line line;
    switch_diag_register_line(&line, 0x70019050, 0x1000, 0x1000);
    EXPECT_STREQ("70019050 00001000 SAME", line.text);
    switch_diag_register_line(&line, 0x70019c0c, 0xffe00000, 0xffd00000);
    EXPECT_STREQ("70019C0C FFE00000 DIFFERENT FFD00000", line.text);
}

TEST(DiagReport, ALineNeverPassesWhatDisplayTextDraws)
{
    switch_diag_line line;
    switch_diag_line_start(&line, "");
    for (int i = 0; i < 40; ++i)
        switch_diag_line_add(&line, "ABCDEFGH");
    EXPECT_EQ(SWITCH_DIAG_LINE_MAX, line.length);
    EXPECT_EQ(SWITCH_DIAG_LINE_MAX, std::strlen(line.text));
    switch_diag_line_hex(&line, 0xffff, 8);
    EXPECT_EQ(SWITCH_DIAG_LINE_MAX, std::strlen(line.text));
}

TEST(DiagReport, AClippedLineStopsWhereTheColumnEnds)
{
    switch_diag_line line;
    switch_diag_line_start(&line, "ABCDEFGH");
    switch_diag_line_clip(&line, 3);
    EXPECT_STREQ("ABC", line.text);
    EXPECT_EQ(3u, line.length);
    switch_diag_line_clip(&line, 10);
    EXPECT_STREQ("ABC", line.text);
    switch_diag_line_clip(&line, 0);
    EXPECT_STREQ("", line.text);
}

TEST(DiagTables, TwentyFiveRegistersAndSixGicWords)
{
    EXPECT_EQ(25u, SWITCH_DIAG_MC_COUNT);
    EXPECT_EQ(6u, SWITCH_DIAG_GIC_COUNT);
    for (unsigned i = 0; i < SWITCH_DIAG_MC_COUNT; ++i)
        EXPECT_EQ(0x70019000u, switch_diag_mc[i][0] & 0xfffff000u) << i;
}
