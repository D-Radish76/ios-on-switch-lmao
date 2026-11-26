// The carveout table as the loader is built (no SWITCH_CARVEOUT_ALL_ROWS): a row that does not boot has no values, and the
// Erista's row still refuses before native_gate (loader.c: refuse 0x306 on `boot 0`).
#include <gtest/gtest.h>

#include "../carveout.h"
#include "kits/SwitchBoard/SwitchBoard.h"

namespace {

uint32_t Zero(uint32_t)
{
    return 0;
}

TEST(CarveoutGate, TheEristaIsNotBootingAndHasNoValuesInTheLoader)
{
    const switch_board *erista = switch_board_by_name("erista");
    EXPECT_EQ(erista->boot, 0);
    EXPECT_EQ(erista->features, 0u);
    EXPECT_TRUE(switch_carveout_for(erista) == nullptr);
    EXPECT_EQ(switch_carveout_check(erista, Zero), 0x402u);
}

TEST(CarveoutGate, AConsoleTheRegistersNameAsTheEristaDoesNotBoot)
{
    const switch_board *detected = switch_board_detect(0x00022117, 0x00000204);
    EXPECT_TRUE(switch_board_streq(detected->name, "erista"));
    EXPECT_EQ(detected->boot, 0);
}

TEST(CarveoutGate, TheOledStillHasItsValues)
{
    const switch_board *oled = switch_board_by_name("oled");
    EXPECT_EQ(oled->boot, 1);
    EXPECT_TRUE(switch_carveout_for(oled) == switch_carveout_OLED);
}

TEST(CarveoutGate, AnUnknownConsoleDoesNotBootAndHasNoValues)
{
    const switch_board *board = switch_board_detect(0x00000000, 0x00000000);
    EXPECT_EQ(board->boot, 0);
    EXPECT_EQ(board->features, 0u);
    EXPECT_TRUE(switch_carveout_for(board) == nullptr);
}

TEST(CarveoutGate, EveryBootingRowHasValues)
{
    for (const switch_board &board : switch_boards)
        EXPECT_EQ(board.boot != 0, switch_carveout_for(&board) != nullptr) << board.name;
}

}  // namespace
