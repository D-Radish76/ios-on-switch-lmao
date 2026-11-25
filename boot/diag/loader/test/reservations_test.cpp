// D0-R's screen lines and its overlap and bank decisions (reservations.c), on the host.
#include <gtest/gtest.h>

#include <string>

extern "C" {
#include "../reservations.h"
}

namespace {
switch_ram_ledger ledger(std::initializer_list<std::pair<uint64_t, uint64_t>> ranges)
{
    switch_ram_ledger l = {};
    l.banks[0] = {0x80000000, 0xffd00000};
    l.banks[1] = {0x100000000, 0x180000000};
    for (auto [start, end] : ranges)
        l.reserved[l.count++] = {start, end};
    return l;
}

std::string line(const char *label, uint64_t start, uint64_t end)
{
    switch_diag_line l;
    switch_diag_range_line(&l, label, start, end);
    return l.text;
}
}  // namespace

TEST(Lines, a_range_shows_start_size_and_end_in_ten_digits)
{
    EXPECT_EQ(line("MAP 0", 0x80000000, 0x80020000), "MAP 0 0080000000 0000020000 0080020000");
    EXPECT_EQ(line("BANK 1", 0x100000000, 0x180000000), "BANK 1 0100000000 0080000000 0180000000");
}

TEST(Lines, the_longest_line_fits_the_left_column)
{
    EXPECT_LE(line("NODE 3", 0xfffffffffe, 0xffffffffff).size(), 58u);
}

TEST(Lines, a_range_that_does_not_fit_forty_bits_or_runs_backwards_is_said_so_not_cut)
{
    EXPECT_EQ(line("MAP 2", 0x10000000000, 0x10000001000), "MAP 2 BEYOND 40 BITS");
    EXPECT_EQ(line("MAP 2", 0x2000, 0x1000), "MAP 2 BEYOND 40 BITS");
}

TEST(Overlap, finds_the_first_range_that_touches_and_none_when_apart)
{
    const switch_ram_ledger l = ledger({{0x80000000, 0x80020000}, {0xb0000000, 0xb0200000}});
    EXPECT_EQ(switch_diag_first_overlap(&l, 0xa7000000, 0xa7010000), -1);
    EXPECT_EQ(switch_diag_first_overlap(&l, 0xa001f000, 0xa0020000), -1);
    EXPECT_EQ(switch_diag_first_overlap(&l, 0xafffffff, 0xb0000001), 1);
    EXPECT_EQ(switch_diag_first_overlap(&l, 0x7ffff000, 0x80001000), 0);
}

TEST(Overlap, touching_ends_do_not_overlap)
{
    const switch_ram_ledger l = ledger({{0xa7010000, 0xa7020000}});
    EXPECT_EQ(switch_diag_first_overlap(&l, 0xa7000000, 0xa7010000), -1);
    EXPECT_EQ(switch_diag_first_overlap(&l, 0xa6ff0000, 0xa7000000), -1);
}

TEST(Overlap, a_count_past_the_table_is_not_read_past_it)
{
    switch_ram_ledger l = ledger({});
    l.count = SWITCH_RAM_RANGES + 5;
    EXPECT_EQ(switch_diag_first_overlap(&l, 0xa7000000, 0xa7010000), -1);
}

TEST(Banks, an_area_is_held_by_the_bank_that_contains_all_of_it)
{
    const switch_ram_ledger l = ledger({});
    EXPECT_EQ(switch_diag_bank_holding(&l, 0xa7000000, 0xa7010000), 0);
    EXPECT_EQ(switch_diag_bank_holding(&l, 0x100000000, 0x100001000), 1);
    EXPECT_EQ(switch_diag_bank_holding(&l, 0xffcff000, 0xffd01000), -1);
    EXPECT_EQ(switch_diag_bank_holding(&l, 0xffd00000, 0xffd01000), -1);
    EXPECT_EQ(switch_diag_bank_holding(&l, 0xa7000000, 0xa7000000), -1);
}

TEST(Areas, the_exporter_and_sd_areas_sit_in_bank_zero_apart_from_the_loader)
{
    EXPECT_EQ(SWITCH_DIAG_EXPORTER_END - SWITCH_DIAG_EXPORTER_START, 0x10000u);
    const switch_ram_ledger l = ledger({});
    EXPECT_EQ(switch_diag_bank_holding(&l, SWITCH_DIAG_EXPORTER_START, SWITCH_DIAG_EXPORTER_END), 0);
}
