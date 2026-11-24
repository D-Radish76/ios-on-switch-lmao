// The loader's board word on the command line (kits/SwitchBoard, boot/loader/loader.c): after bootargs.txt, whatever
// the card's file says, and readable the way XNU reads a boot-arg (the last of two equal ones wins).
#include <gtest/gtest.h>

#include <string>
#include <vector>

extern "C" {
#include "../boot_args.h"
}
#include "kits/SwitchBoard/SwitchBoard.h"

namespace {

std::string Line(const std::string &builtin, const std::string &file, const switch_board *board)
{
    std::vector<char> line(SWITCH_COMMAND_LINE_BYTES, 0);
    std::copy(builtin.begin(), builtin.end(), line.begin());
    std::vector<char> page(SWITCH_BOOT_ARGS_BYTES, 0);
    std::copy(file.begin(), file.end(), page.begin());
    int verbose = 0;
    EXPECT_EQ(switch_boot_args_append(line.data(), page.data(), SWITCH_BOOT_ARGS_BYTES, &verbose), SWITCH_BOOT_ARGS_OK);
    char word[SWITCH_BOARD_WORD_BYTES];
    const size_t bytes = switch_board_word(word, sizeof(word), board);
    EXPECT_GT(bytes, 0u);
    EXPECT_EQ(word[bytes - 1], 0);
    EXPECT_EQ(switch_boot_args_append(line.data(), word, static_cast<uint32_t>(bytes), &verbose), SWITCH_BOOT_ARGS_OK);
    return std::string(line.data());
}

// What PE_parse_boot_arg_str(switch-board) returns for a line: XNU's loop keeps going, so the last one wins.
std::string KernelReads(const std::string &line)
{
    std::string name;
    size_t at = 0;
    while (at < line.size()) {
        const size_t end = line.find(' ', at) == std::string::npos ? line.size() : line.find(' ', at);
        const std::string word = line.substr(at, end - at);
        if (word.rfind("switch-board=", 0) == 0)
            name = word.substr(13);
        at = end + 1;
    }
    return name;
}

TEST(LoaderBoardArgs, TheWordFollowsTheBuiltInLineAndTheFile)
{
    EXPECT_EQ(Line("debug=0x14e", "-v switch-no-bt", switch_board_by_name("oled")),
              "debug=0x14e -v switch-no-bt switch-board=oled");
    EXPECT_EQ(Line("debug=0x14e", "", switch_board_by_name("oled")), "debug=0x14e switch-board=oled");
}

TEST(LoaderBoardArgs, TheCardsFileCannotNameAnotherBoard)
{
    const std::string line = Line("debug=0x14e", "switch-board=erista\nswitch-board=unknown", switch_board_by_name("oled"));
    EXPECT_EQ(KernelReads(line), "oled");
}

TEST(LoaderBoardArgs, EveryBoardsNameFits)
{
    for (const switch_board &board : switch_boards) {
        char word[SWITCH_BOARD_WORD_BYTES];
        EXPECT_GT(switch_board_word(word, sizeof(word), &board), 0u) << board.name;
    }
    char small[8];
    EXPECT_EQ(switch_board_word(small, sizeof(small), switch_board_by_name("oled")), 0u);
}
}  // namespace
