// kits/SwitchBoard: how a board is told apart, what each row allows, and what the kernel reads back from the loader.
#include "kits/SwitchBoard/SwitchBoardKernel.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {
std::string gCommandLine;

uint32_t hidrev(unsigned major)
{
    return 0x21u << 8 | major << 4 | 1;
}

uint32_t odm4(unsigned type)
{
    return type << 16;
}

const switch_board *identify(uint32_t hidrevValue, uint32_t odm4Value)
{
    return switch_board_detect(hidrevValue, odm4Value);
}

TEST(SwitchBoard, TheOledBootsWithEveryFeature)
{
    const switch_board *board = identify(hidrev(2), odm4(4));
    ASSERT_STREQ(board->name, "oled");
    EXPECT_EQ(board->boot, 1);
    EXPECT_EQ(board->features, SWITCH_FEATURES_ALL);
    EXPECT_EQ(board->t210b01, 1);
    EXPECT_EQ(board->sku, 3);
}

TEST(SwitchBoard, TheEristaIsKnownButDoesNotBootAndHasNoFeatures)
{
    const switch_board *board = identify(hidrev(1), 0);
    ASSERT_STREQ(board->name, "erista");
    EXPECT_EQ(board->boot, 0);
    EXPECT_EQ(board->features, 0u);
}

TEST(SwitchBoard, OtherSkusAndChipsAreUnknown)
{
    for (unsigned type : {0u, 1u, 2u, 3u, 5u, 15u}) {
        const switch_board *board = identify(hidrev(2), odm4(type));
        if (type == 4)
            continue;
        EXPECT_STREQ(board->name, "unknown") << type;
        EXPECT_EQ(board->boot, 0);
        EXPECT_EQ(board->features, 0u);
    }
    EXPECT_STREQ(identify(hidrev(3), odm4(4))->name, "unknown");
    EXPECT_STREQ(identify(hidrev(0), 0)->name, "unknown");
    EXPECT_STREQ(identify(0x18 << 8 | 1 << 4, 0)->name, "unknown");  // not a T210 family chip
    EXPECT_STREQ(identify(0, 0)->name, "unknown");
    EXPECT_STREQ(identify(0xffffffff, 0xffffffff)->name, "unknown");
}

TEST(SwitchBoard, TheEristaSkuIgnoresTheFuse)
{
    uint32_t t210b01 = 9, sku = 9;
    switch_board_identify(hidrev(1), odm4(4), &t210b01, &sku);
    EXPECT_EQ(t210b01, 0u);
    EXPECT_EQ(sku, 0u);
}

TEST(SwitchBoard, UBootsRulesForTheT210B01Skus)
{
    const struct {
        unsigned type, sku;
    } rows[] = {{0, 1}, {1, 1}, {2, 2}, {3, 1}, {4, 3}, {5, 1}, {15, 1}};
    for (const auto &row : rows) {
        uint32_t t210b01 = 0, sku = 9;
        switch_board_identify(hidrev(2), odm4(row.type), &t210b01, &sku);
        EXPECT_EQ(t210b01, 1u);
        EXPECT_EQ(sku, row.sku) << row.type;
    }
}

TEST(SwitchBoard, NoTwoBoardsShareAnIdentity)
{
    const size_t count = sizeof(switch_boards) / sizeof(switch_boards[0]);
    for (size_t i = 0; i < count; i++)
        for (size_t j = i + 1; j < count; j++) {
            EXPECT_STRNE(switch_boards[i].name, switch_boards[j].name);
            EXPECT_FALSE(switch_boards[i].t210b01 == switch_boards[j].t210b01 && switch_boards[i].sku == switch_boards[j].sku);
        }
    for (size_t i = 0; i < count; i++) {
        EXPECT_LT(std::strlen(switch_boards[i].name), static_cast<size_t>(SWITCH_BOARD_NAME_BYTES));
        EXPECT_EQ(switch_boards[i].features & ~SWITCH_FEATURES_ALL, 0u);
    }
}

TEST(SwitchBoard, TheKernelReadsTheLoadersWord)
{
    gCommandLine = "-v switch-board=oled";
    EXPECT_TRUE(switch_board_has(SWITCH_FEATURE_CHARGER));
    EXPECT_TRUE(switch_board_has(SWITCH_FEATURE_SLEEP));

    gCommandLine = "-v switch-board=erista";
    for (uint32_t bit = 1; bit <= SWITCH_FEATURE_SE2; bit <<= 1)
        EXPECT_FALSE(switch_board_has(static_cast<switch_feature>(bit))) << bit;
}

TEST(SwitchBoard, AWordThatIsNotInTheTableEnablesNothing)
{
    for (const char *line : {"switch-board=", "switch-board=oledx", "switch-board=OLED", "switch-board=ole", "switch-board=zzz"}) {
        gCommandLine = line;
        for (uint32_t bit = 1; bit <= SWITCH_FEATURE_SE2; bit <<= 1)
            EXPECT_FALSE(switch_board_has(static_cast<switch_feature>(bit))) << line << " " << bit;
    }
}

TEST(SwitchBoard, TheLastWordWinsSoBootargsTxtCannotOverrideTheLoader)
{
    gCommandLine = "switch-board=oled switch-board=erista";
    EXPECT_FALSE(switch_board_has(SWITCH_FEATURE_CHARGER));
    gCommandLine = "switch-board=erista bootargs switch-board=oled";
    EXPECT_TRUE(switch_board_has(SWITCH_FEATURE_CHARGER));
}

TEST(SwitchBoard, NoWordIsAKernelNotStartedByTheLoader)
{
    gCommandLine = "-v switch-no-bt";
    EXPECT_TRUE(switch_board_has(SWITCH_FEATURE_GPU));
    EXPECT_STREQ(switch_board_current()->name, "oled");
}

TEST(SwitchBoard, TheOledNamesItselfAndNoOtherBoardGuesses)
{
    const switch_board *oled = switch_board_by_name("oled");
    EXPECT_STREQ(oled->model, "Nintendo Switch (OLED model)");
    EXPECT_STREQ(oled->compatible, "nintendo,aula");
    EXPECT_EQ(switch_board_by_name("erista")->model, nullptr);
    EXPECT_EQ(switch_board_by_name("erista")->compatible, nullptr);
    EXPECT_EQ(switch_board_by_name("nope")->model, nullptr);
}

TEST(SwitchBoard, TheOledMaskCoversEveryFeatureAndNothingElse)
{
    EXPECT_EQ(SWITCH_FEATURES_ALL, (SWITCH_FEATURE_SE2 << 1) - 1);
}

TEST(SwitchBoard, AFeatureNeedsEveryBitAsked)
{
    const switch_board partial = {"partial", 9, 9, 0, SWITCH_FEATURE_GPU, nullptr, nullptr};
    EXPECT_TRUE(switch_board_has_feature(&partial, SWITCH_FEATURE_GPU));
    EXPECT_FALSE(switch_board_has_feature(&partial, static_cast<switch_feature>(SWITCH_FEATURE_GPU | SWITCH_FEATURE_VIDEO)));
}
}  // namespace

// The last occurrence wins and a name longer than the buffer is cut, as XNU's PE_parse_boot_argn_internal does.
extern "C" int PE_parse_boot_arg_str(const char *name, char *value, int size)
{
    const std::string key = std::string(name) + "=";
    bool found = false;
    size_t at = 0;
    while ((at = gCommandLine.find(key, at)) != std::string::npos) {
        if (at == 0 || gCommandLine[at - 1] == ' ') {
            const size_t start = at + key.size();
            const std::string word = gCommandLine.substr(start, gCommandLine.find(' ', start) - start);
            std::strncpy(value, word.c_str(), static_cast<size_t>(size - 1));
            value[size - 1] = 0;
            found = true;
        }
        at += key.size();
    }
    return found;
}
