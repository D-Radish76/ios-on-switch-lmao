// kits/SwitchBoard: how a board is told apart, what each row allows, and what the kernel reads back from the loader.
#include "kits/SwitchBoard/SwitchBoardKernel.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {
std::string gCommandLine;
uint32_t gHidrev, gOdm4;
std::string gCompatible;  // the device tree property, with its zeros
bool gNoRegisters, gNoDeviceTree;

void oledConsole(const char *commandLine = "")
{
    gCommandLine = commandLine;
    gHidrev = 0x21u << 8 | 2u << 4 | 1;
    gOdm4 = 4u << 16;
    static const char list[] = "nvidia,fric\0nintendo,aula\0nintendo,nx\0nvidia,tegra210b01";
    gCompatible.assign(list, sizeof(list));
    gNoRegisters = gNoDeviceTree = false;
}

void expectNoFeatures(const char *why)
{
    for (uint32_t bit = 1; bit <= SWITCH_FEATURE_CACHE_IDLE; bit <<= 1)
        EXPECT_FALSE(switch_board_has(static_cast<switch_feature>(bit))) << why << " " << bit;
}

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
    oledConsole("-v switch-board=oled");
    EXPECT_TRUE(switch_board_has(SWITCH_FEATURE_CHARGER));
    EXPECT_TRUE(switch_board_has(SWITCH_FEATURE_SLEEP));
    EXPECT_TRUE(switch_board_has(SWITCH_FEATURE_RTC_MARK));
}

TEST(SwitchBoard, AnEristaHasNoFeaturesWhateverTheWord)
{
    for (const char *line : {"switch-board=erista", "", "switch-board=oled"}) {
        oledConsole(line);
        gHidrev = 0x21u << 8 | 1u << 4 | 1;
        gOdm4 = 0;
        gCompatible.assign("nvidia,tegra210", 16);
        expectNoFeatures(line);
    }
}

TEST(SwitchBoard, AWordThatIsNotInTheTableEnablesNothing)
{
    for (const char *line : {"switch-board=", "switch-board=oledx", "switch-board=OLED", "switch-board=ole", "switch-board=zzz",
                             "switch-board=unknown"}) {
        oledConsole(line);
        expectNoFeatures(line);
    }
}

TEST(SwitchBoard, TheLastWordWinsSoBootargsTxtCannotOverrideTheLoader)
{
    oledConsole("switch-board=oled switch-board=erista");
    EXPECT_FALSE(switch_board_has(SWITCH_FEATURE_CHARGER));
    oledConsole("switch-board=erista bootargs switch-board=oled");
    EXPECT_TRUE(switch_board_has(SWITCH_FEATURE_CHARGER));
}

TEST(SwitchBoard, ADuplicateThatDisagreesOrIsEmptyEnablesNothing)
{
    for (const char *line : {"switch-board=oled switch-board=", "switch-board=oled switch-board=bogus",
                             "switch-board=erista switch-board=erista", "switch-board=oled switch-board=oled switch-board="}) {
        oledConsole(line);
        expectNoFeatures(line);
    }
    oledConsole("switch-board=oled switch-board=oled");
    EXPECT_TRUE(switch_board_has(SWITCH_FEATURE_GPU));
}

TEST(SwitchBoard, AWordNeverMatchesOnlyBecauseItWasCut)
{
    const std::string valid = "switch-board=oled";
    for (const char *tail : {"AAAAAAAAAAA", "AAAAAAAAAAAA", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}) {
        oledConsole((valid + tail).c_str());
        expectNoFeatures(tail);
    }
    oledConsole("xswitch-board=oled");  // another key that ends the same way
    EXPECT_TRUE(switch_board_has(SWITCH_FEATURE_GPU));  // no word: the registers speak
    oledConsole("switch-board=oled");
    EXPECT_TRUE(switch_board_has(SWITCH_FEATURE_GPU));
}

TEST(SwitchBoard, NoWordIsTheOledOnlyWhereTheRegistersSayOled)
{
    oledConsole("-v switch-no-bt");
    EXPECT_TRUE(switch_board_has(SWITCH_FEATURE_GPU));
    EXPECT_STREQ(switch_board_current()->name, "oled");
    for (uint32_t odm4Value : {odm4(2), odm4(0), odm4(15)}) {  // a T210B01 that is not the OLED sku
        oledConsole();
        gOdm4 = odm4Value;
        expectNoFeatures("other sku");
    }
    oledConsole();
    gHidrev = hidrev(2) & ~(0xffu << 8);  // not a T210 family chip
    expectNoFeatures("other chip");
}

TEST(SwitchBoard, MissingRegistersOrDeviceTreeEnableNothing)
{
    oledConsole("switch-board=oled");
    gNoRegisters = true;
    expectNoFeatures("no registers");
    oledConsole();
    gNoDeviceTree = true;
    expectNoFeatures("no device tree");
}

TEST(SwitchBoard, TheWordAndTheRegistersMustAgree)
{
    oledConsole("switch-board=erista");  // the registers say OLED
    expectNoFeatures("word erista, registers oled");
    oledConsole("switch-board=oled");
    gOdm4 = odm4(2);  // the registers say another T210B01 sku
    expectNoFeatures("word oled, registers other sku");
}

TEST(SwitchBoard, TheDeviceTreeMustNameTheRow)
{
    for (const char *compatible : {"nintendo,aulax", "nintendo,aul", "nvidia,tegra210b01", ""}) {
        oledConsole("switch-board=oled");
        gCompatible = std::string(compatible, strlen(compatible) + 1);
        expectNoFeatures(compatible);
    }
    oledConsole("switch-board=oled");
    gCompatible = "nintendo,aula";  // no terminating zero in the property: cut off
    gCompatible.pop_back();
    expectNoFeatures("not terminated");
    oledConsole("switch-board=oled");
    gCompatible.assign("a\0nintendo,aula", 16);  // the first entry is not the only one
    EXPECT_TRUE(switch_board_has(SWITCH_FEATURE_GPU));
}

TEST(SwitchBoard, AnEristaThatAllThreeWitnessesNameIsKnownButHasNothingToStart)
{
    static const char list[] = "nvidia,odin\0nintendo,icosa\0nintendo,nx\0nvidia,tegra210";
    const switch_board *board = switch_board_resolve("erista", 0x21u << 8 | 1u << 4, 0, list, sizeof(list));
    EXPECT_STREQ(board->name, "erista");
    EXPECT_EQ(board->boot, 0);
    EXPECT_EQ(board->features, 0u);
    EXPECT_EQ(switch_board_resolve("erista", 0x21u << 8 | 1u << 4, 0, "nintendo,aula", 14), &switch_board_unknown);
    EXPECT_EQ(switch_board_resolve("oled", 0x21u << 8 | 1u << 4, 0, list, sizeof(list)), &switch_board_unknown);
}

TEST(SwitchBoard, TheOledNamesItselfAndNoOtherBoardGuesses)
{
    const switch_board *oled = switch_board_by_name("oled");
    EXPECT_STREQ(oled->model, "Nintendo Switch (OLED model)");
    EXPECT_STREQ(oled->compatible, "nintendo,aula");
    EXPECT_STREQ(switch_board_by_name("erista")->model, "Nintendo Switch (2017)");
    EXPECT_STREQ(switch_board_by_name("erista")->compatible, "nintendo,icosa");
    EXPECT_EQ(switch_board_by_name("nope")->model, nullptr);
}

TEST(SwitchBoard, TheDeviceTreeAloneNamesTheRowForTheCpuIdleCheck)
{
    oledConsole();
    EXPECT_TRUE(switch_board_has_feature(switch_board_by_compatible(gCompatible.data(), gCompatible.size()), SWITCH_FEATURE_CACHE_IDLE));
    for (const char *other : {"nvidia,tegra210", "nintendo,aulax", ""}) {
        const std::string list(other, strlen(other) + 1);
        EXPECT_FALSE(switch_board_has_feature(switch_board_by_compatible(list.data(), list.size()), SWITCH_FEATURE_CACHE_IDLE));
    }
    EXPECT_FALSE(switch_board_has_feature(switch_board_by_compatible("nintendo,aula", 13), SWITCH_FEATURE_CACHE_IDLE));
    EXPECT_FALSE(switch_board_has_feature(switch_board_by_compatible(nullptr, 0), SWITCH_FEATURE_CACHE_IDLE));
}

TEST(SwitchBoard, TheOledMaskCoversEveryFeatureAndNothingElse)
{
    EXPECT_EQ(SWITCH_FEATURES_ALL, (SWITCH_FEATURE_CACHE_IDLE << 1) - 1);
}

TEST(SwitchBoard, AFeatureNeedsEveryBitAsked)
{
    const switch_board partial = {"partial", 9, 9, 0, SWITCH_FEATURE_GPU, nullptr, nullptr};
    EXPECT_TRUE(switch_board_has_feature(&partial, SWITCH_FEATURE_GPU));
    EXPECT_FALSE(switch_board_has_feature(&partial, static_cast<switch_feature>(SWITCH_FEATURE_GPU | SWITCH_FEATURE_VIDEO)));
}
}  // namespace

extern "C" int switch_board_read_hardware(uint32_t *hidrevValue, uint32_t *odm4Value)
{
    if (gNoRegisters)
        return -1;
    *hidrevValue = gHidrev;
    *odm4Value = gOdm4;
    return 0;
}

extern "C" int switch_board_read_compatible(const char **list, size_t *bytes)
{
    if (gNoDeviceTree)
        return -1;
    *list = gCompatible.data();
    *bytes = gCompatible.size();
    return 0;
}

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
