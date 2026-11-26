// native_gate's carveout check (boot/loader/carveout.h) against the Erista's measured registers and the OLED's table.
#include <gtest/gtest.h>

#include <map>

#include "../carveout.h"
#include "erista_mc_fixture.h"
#include "kits/SwitchBoard/SwitchBoard.h"

namespace {

std::map<uint32_t, uint32_t> g_registers;
unsigned g_reads;

uint32_t Read(uint32_t address)
{
    ++g_reads;
    return g_registers.at(address);
}

void LoadErista()
{
    g_registers.clear();
    g_reads = 0;
    for (const MeasuredRegister &r : kEristaMeasured)
        g_registers[r.address] = r.value;
}

void LoadOled()
{
    g_registers.clear();
    g_reads = 0;
    for (const auto &row : switch_carveout_oled)
        g_registers[row[0]] = row[1];
}

TEST(Carveout, TheEristasMeasuredRegistersPass)
{
    LoadErista();
    EXPECT_EQ(switch_carveout_check("erista", Read), 0u);
    EXPECT_EQ(g_reads, 25u);
}

TEST(Carveout, TheEristaTableIsTheMeasurementInOrder)
{
    ASSERT_EQ(sizeof(kEristaMeasured) / sizeof(kEristaMeasured[0]), 25u);
    ASSERT_EQ(sizeof(switch_carveout_erista) / sizeof(switch_carveout_erista[0]), 25u);
    for (unsigned i = 0; i < 25; ++i) {
        EXPECT_EQ(switch_carveout_erista[i][0], kEristaMeasured[i].address) << i;
        EXPECT_EQ(switch_carveout_erista[i][1], kEristaMeasured[i].value) << i;
    }
}

TEST(Carveout, EachEristaRegisterThatDiffersIsRefusedAtItsOwnIndex)
{
    for (unsigned i = 0; i < 25; ++i) {
        LoadErista();
        g_registers[kEristaMeasured[i].address] ^= 1u;
        EXPECT_EQ(switch_carveout_check("erista", Read), 0x410u + i) << i;
    }
}

TEST(Carveout, EachOledRegisterThatDiffersIsRefusedAtItsOwnIndex)
{
    for (unsigned i = 0; i < 25; ++i) {
        LoadOled();
        g_registers[switch_carveout_oled[i][0]] ^= 0x80000000u;
        EXPECT_EQ(switch_carveout_check("oled", Read), 0x410u + i) << i;
    }
}

TEST(Carveout, TheOledPassesOnItsOwnTable)
{
    LoadOled();
    EXPECT_EQ(switch_carveout_check("oled", Read), 0u);
    EXPECT_EQ(g_reads, 25u);
}

TEST(Carveout, TheOledTableIsTheOneNativeGateHadBeforeTheSplit)
{
    const uint32_t expected[][2] = {
        {0x70019050, 0x1000},     {0x70019670, 0},          {0x70019674, 0},          {0x700199d4, 0},
        {0x700199a0, 0xfff00000}, {0x700199a4, 0},          {0x700199a8, 0},          {0x70019648, 0},
        {0x7001964c, 0},          {0x70019978, 0},          {0x70019c0c, 0xffe00000}, {0x70019c10, 0},
        {0x70019c14, 8},          {0x70019c5c, 0xffd00000}, {0x70019c60, 0},          {0x70019c64, 2},
        {0x70019cac, 0xffd40000}, {0x70019cb0, 0},          {0x70019cb4, 6},          {0x70019cfc, 0},
        {0x70019d00, 0},          {0x70019d04, 0},          {0x70019d4c, 0},          {0x70019d50, 0},
        {0x70019d54, 0},
    };
    for (unsigned i = 0; i < 25; ++i) {
        EXPECT_EQ(switch_carveout_oled[i][0], expected[i][0]) << i;
        EXPECT_EQ(switch_carveout_oled[i][1], expected[i][1]) << i;
    }
}

TEST(Carveout, AnUnknownBoardIsRefusedWithoutReadingAnything)
{
    LoadErista();
    for (const char *name : {"unknown", "", "OLED", "oled ", "erista2", "ole"}) {
        EXPECT_EQ(switch_carveout_check(name, Read), 0x402u) << name;
    }
    EXPECT_EQ(g_reads, 0u);
    EXPECT_TRUE(switch_carveout_table("unknown") == nullptr);
}

TEST(Carveout, TheTablesAreSeparateStorage)
{
    EXPECT_NE(static_cast<const void *>(switch_carveout_table("oled")),
              static_cast<const void *>(switch_carveout_table("erista")));
}

TEST(Carveout, AValueOfTheOtherBoardDoesNotStandInForThisOne)
{
    LoadErista();
    g_registers[0x700199a0] = 0xfff00000u + 0x100000;
    EXPECT_EQ(switch_carveout_check("oled", Read), 0x414u);
    EXPECT_EQ(switch_carveout_check("erista", Read), 0x414u);
}

TEST(Carveout, EveryBoardInTheTableHasACarveoutTableOrIsRefused)
{
    for (const switch_board &board : switch_boards) {
        const bool has = switch_carveout_table(board.name) != nullptr;
        LoadErista();
        EXPECT_EQ(switch_carveout_check(board.name, Read) == 0x402u, !has) << board.name;
    }
}

TEST(Carveout, TheEristaStillDoesNotBootAndHasNoFeatures)
{
    const switch_board *erista = switch_board_by_name("erista");
    EXPECT_TRUE(erista != nullptr);
    if (!erista)
        return;
    EXPECT_EQ(erista->boot, 0);
    EXPECT_EQ(erista->features, 0u);
    const switch_board *detected = switch_board_detect(0x00022117, 0x00000204);
    EXPECT_TRUE(switch_board_streq(detected->name, "erista"));
    EXPECT_EQ(detected->boot, 0);
    const switch_board *oled = switch_board_by_name("oled");
    EXPECT_TRUE(oled != nullptr);
    if (!oled)
        return;
    EXPECT_EQ(oled->boot, 1);
}

TEST(Carveout, AnUnknownConsoleIsDetectedAsABoardThatDoesNotBoot)
{
    const switch_board *board = switch_board_detect(0x00000000, 0x00000000);
    EXPECT_EQ(board->boot, 0);
    EXPECT_EQ(board->features, 0u);
}

}  // namespace
