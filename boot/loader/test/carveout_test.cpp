// native_gate's carveout check (boot/loader/carveout.h) with every board's values present, against the Erista's measured
// registers and the OLED's table. carveout_gate_test.cpp has the table as the loader is built.
#define SWITCH_CARVEOUT_ALL_ROWS 1
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

void Load(const uint32_t *values)
{
    g_registers.clear();
    g_reads = 0;
    for (unsigned i = 0; i < 25; ++i)
        g_registers[SWITCH_CARVEOUT_MC_BASE + switch_carveout_offset[i]] = values[i];
}

void LoadErista()
{
    g_registers.clear();
    g_reads = 0;
    for (const MeasuredRegister &r : kEristaMeasured)
        g_registers[r.address] = r.value;
}

const switch_board *Oled()
{
    return switch_board_by_name("oled");
}

const switch_board *Erista()
{
    return switch_board_by_name("erista");
}

TEST(Carveout, TheEristasMeasuredRegistersPass)
{
    LoadErista();
    EXPECT_EQ(switch_carveout_check(Erista(), Read), 0u);
    EXPECT_EQ(g_reads, 25u);
}

TEST(Carveout, TheEristaValuesAreTheMeasurementInOrder)
{
    for (unsigned i = 0; i < 25; ++i) {
        EXPECT_EQ(SWITCH_CARVEOUT_MC_BASE + switch_carveout_offset[i], kEristaMeasured[i].address) << i;
        EXPECT_EQ(switch_carveout_ERISTA[i], kEristaMeasured[i].value) << i;
    }
}

TEST(Carveout, EachBoardsRowIsItsOwnValues)
{
    EXPECT_TRUE(switch_carveout_for(Oled()) == switch_carveout_OLED);
    EXPECT_TRUE(switch_carveout_for(Erista()) == switch_carveout_ERISTA);
    EXPECT_TRUE(switch_carveout_for(Oled()) != switch_carveout_for(Erista()));
}

TEST(Carveout, EachEristaRegisterThatDiffersIsRefusedAtItsOwnIndex)
{
    for (unsigned i = 0; i < 25; ++i) {
        LoadErista();
        g_registers[kEristaMeasured[i].address] ^= 1u;
        EXPECT_EQ(switch_carveout_check(Erista(), Read), 0x410u + i) << i;
    }
}

TEST(Carveout, EachOledRegisterThatDiffersIsRefusedAtItsOwnIndex)
{
    for (unsigned i = 0; i < 25; ++i) {
        Load(switch_carveout_OLED);
        g_registers[SWITCH_CARVEOUT_MC_BASE + switch_carveout_offset[i]] ^= 0x80000000u;
        EXPECT_EQ(switch_carveout_check(Oled(), Read), 0x410u + i) << i;
    }
}

TEST(Carveout, TheOledPassesOnItsOwnValues)
{
    Load(switch_carveout_OLED);
    EXPECT_EQ(switch_carveout_check(Oled(), Read), 0u);
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
        EXPECT_EQ(SWITCH_CARVEOUT_MC_BASE + switch_carveout_offset[i], expected[i][0]) << i;
        EXPECT_EQ(switch_carveout_OLED[i], expected[i][1]) << i;
    }
}

TEST(Carveout, AnUnknownBoardIsRefusedWithoutReadingAnything)
{
    LoadErista();
    EXPECT_EQ(switch_carveout_check(&switch_board_unknown, Read), 0x402u);
    EXPECT_EQ(switch_carveout_check(switch_board_by_name("lite"), Read), 0x402u);
    EXPECT_EQ(g_reads, 0u);
    EXPECT_TRUE(switch_carveout_for(&switch_board_unknown) == nullptr);
}

TEST(Carveout, ACopyOfARowIsNotThatRow)
{
    switch_board copy = *Oled();
    EXPECT_EQ(switch_carveout_check(&copy, Read), 0x402u);
    EXPECT_TRUE(switch_carveout_for(nullptr) == nullptr);
}

TEST(Carveout, EveryRowHasValuesAndTheyMatchTheRowsName)
{
    unsigned rows = 0;
    for (const switch_board &board : switch_boards) {
        const uint32_t *values = switch_carveout_for(&board);
        EXPECT_TRUE(values != nullptr) << board.name;
        if (!values)
            continue;
        EXPECT_TRUE(values == (switch_board_streq(board.name, "oled") ? switch_carveout_OLED : switch_carveout_ERISTA))
            << board.name;
        ++rows;
    }
    EXPECT_EQ(rows, 2u);
}

TEST(Carveout, AValueOfTheOtherBoardDoesNotStandInForThisOne)
{
    LoadErista();
    g_registers[0x700199a0] = 0xfff00000u + 0x100000;
    EXPECT_EQ(switch_carveout_check(Oled(), Read), 0x414u);
    EXPECT_EQ(switch_carveout_check(Erista(), Read), 0x414u);
}

TEST(Carveout, TheAddressesAreTheMemoryControllersRegisters)
{
    for (unsigned i = 0; i < 25; ++i) {
        EXPECT_EQ((SWITCH_CARVEOUT_MC_BASE + switch_carveout_offset[i]) & 0xfffff000u, 0x70019000u) << i;
        for (unsigned j = i + 1; j < 25; ++j)
            EXPECT_NE(switch_carveout_offset[i], switch_carveout_offset[j]);
    }
}

}  // namespace
