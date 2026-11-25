// D1's report of the status block and the reference page (sd_report.c) on the host.
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>

extern "C" {
#include "../sd_report.h"
}

namespace {
constexpr uint32_t kCrcStored = 0x630d1cb8;  // the pinned exporter's data CRC32
// Marked as synthetic: no real card's CID, CSD or serial number is in this file.
constexpr uint32_t kCid[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};

void put_le32(uint8_t *at, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        at[i] = (uint8_t)(value >> (8 * i));
}

void put_be32(uint8_t *at, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        at[i] = (uint8_t)(value >> (24 - 8 * i));
}

void put_le64(uint8_t *at, uint64_t value)
{
    put_le32(at, (uint32_t)value);
    put_le32(at + 4, (uint32_t)(value >> 32));
}

std::array<uint8_t, 0x100> status_block(unsigned flags = SWITCH_DIAG_SD_FLAG_COUNT)
{
    std::array<uint8_t, 0x100> block = {};
    put_le32(&block[0], SWITCH_DIAG_SD_STATUS_MAGIC);
    put_le32(&block[4], SWITCH_DIAG_SD_STATUS_VERSION);
    put_be32(&block[0x08], kCrcStored);
    put_le32(&block[0x0c], 0x5e50);
    for (unsigned off : {0x10u, 0x14u, 0x18u})
        put_be32(&block[off], kCrcStored);
    for (unsigned i = 0; i < flags; ++i)
        put_le32(&block[SWITCH_DIAG_SD_FLAGS_OFFSET + 4 * i], 1);
    return block;
}

std::array<uint8_t, SWITCH_SD_REFERENCE_BYTES> reference_page()
{
    std::array<uint8_t, SWITCH_SD_REFERENCE_BYTES> page = {};
    const uint32_t header[10] = {SWITCH_SD_REFERENCE_MAGIC, 2, 0x500, 0x200, 0x200, 0x700b0000, 0, 1, 1, 0};
    for (unsigned i = 0; i < 10; ++i)
        put_le32(&page[4 * i], header[i]);
    for (unsigned i = 0; i < 512; ++i)
        page[SWITCH_SD_REFERENCE_DATA_OFFSET + i] = (uint8_t)(i * 7);
    page[SWITCH_SD_REFERENCE_DATA_OFFSET + 510] = 0x55;
    page[SWITCH_SD_REFERENCE_DATA_OFFSET + 511] = 0xaa;
    put_be32(&page[0x28], switch_sd_crc32(&page[SWITCH_SD_REFERENCE_DATA_OFFSET], 512));
    uint8_t *card = &page[SWITCH_SD_CARD_OFFSET];
    const uint32_t fixed[8] = {SWITCH_SD_CARD_MAGIC, 1, 0x100, 1, 0x700b0000, 1, 512, 1};
    for (unsigned i = 0; i < 8; ++i)
        put_le32(card + 4 * i, fixed[i]);
    const uint64_t csize = 0x3b37, sectors = (csize + 1) * 1024;
    put_le64(card + 0x20, sectors);
    put_le64(card + 0x28, sectors * 512);
    put_le32(card + 0x30, 0xc0ff8000);
    put_le32(card + 0x34, 0x1234);
    put_le32(card + 0x38, 0x80000001);
    put_le32(card + 0x40, 0x40000000);
    put_le32(card + 0x44, (9u << 16) | (uint32_t)(csize >> 16));
    put_le32(card + 0x48, (uint32_t)(csize & 0xffff) << 16);
    for (unsigned i = 0; i < 4; ++i)
        put_le32(card + 0x50 + 4 * i, kCid[i]);
    put_le64(card + 0x60, 0xffc87000);
    put_le64(card + 0x68, 0xff885000);
    put_le64(card + 0x70, 0xff885800);
    const uint8_t hash[32] = {0xb4, 0x92, 0xa4, 0x01, 0x75, 0x80, 0xca, 0x11, 0xc9, 0xcd, 0x49, 0x07, 0x5a, 0x89, 0x9a, 0x65,
                              0xf6, 0x20, 0x69, 0xd3, 0x75, 0x06, 0x15, 0x21, 0x7f, 0xc1, 0x03, 0x08, 0x95, 0x4c, 0x9f, 0xaa};
    std::memcpy(card + 0x80, hash, sizeof hash);
    put_le32(card + 0xf0, switch_sd_crc32(card, 0xf0));
    return page;
}

switch_diag_sd_result evaluate(const std::array<uint8_t, 0x100> &status, const std::array<uint8_t, 0x1000> &page)
{
    switch_diag_sd_result result;
    switch_diag_sd_evaluate(status.data(), page.data(), &result);
    return result;
}

std::string text(const switch_diag_sd_result &result, unsigned line)
{
    return result.line[line].text.text;
}

std::string all(const switch_diag_sd_result &result)
{
    std::string joined;
    for (unsigned i = 0; i < SWITCH_DIAG_SD_LINES; ++i)
        joined += text(result, i) + "\n";
    return joined;
}
}  // namespace

TEST(Valid, a_complete_run_reads_ten_of_ten_and_a_valid_reference)
{
    const auto result = evaluate(status_block(), reference_page());
    EXPECT_EQ(result.reference_error, 0);
    EXPECT_EQ(result.flags_set, 10u);
    EXPECT_EQ(text(result, 0), "SD STEPS 10 OF 10. EXPORTER RETURNED 0 PER SCRIPT");
    EXPECT_EQ(text(result, 1), "CRC32 WANT 630D1CB8 BYTES 5E50");
    EXPECT_EQ(text(result, 2), "CRC32 SLOT 630D1CB8 RUN 630D1CB8 GO 630D1CB8");
    EXPECT_EQ(text(result, 3), "SD RECORD AND REFERENCE: VALID");
    EXPECT_EQ(text(result, 4), "CARD SECTORS 00ECE000 OCR C0FF8000 RCA 1234");
    EXPECT_EQ(text(result, 5), "CSD STRUCT 1 FLAGS 80000001 SIG 55AA. ID NOT SHOWN");
    for (unsigned i = 0; i < SWITCH_DIAG_SD_LINES; ++i)
        EXPECT_EQ(result.line[i].bad, 0);
}

TEST(Valid, no_line_carries_the_card_identity_or_a_firmware_address)
{
    const std::string shown = all(evaluate(status_block(), reference_page()));
    for (const char *secret : {"11111111", "22222222", "33333333", "44444444", "FFC87000", "FF885000", "FF885800"})
        EXPECT_EQ(shown.find(secret), std::string::npos);
}

TEST(Valid, every_line_fits_the_left_column)
{
    const auto result = evaluate(status_block(), reference_page());
    for (unsigned i = 0; i < SWITCH_DIAG_SD_LINES; ++i)
        EXPECT_LE(result.line[i].text.length, 58u);
}

TEST(Refused, a_reference_page_without_its_header_is_code_2_and_shows_no_card)
{
    auto page = reference_page();
    page[0] ^= 1;
    const auto result = evaluate(status_block(), page);
    EXPECT_EQ(result.reference_error, SWITCH_SD_REF_HEADER);
    EXPECT_EQ(text(result, 3), "SD RECORD AND REFERENCE: FAILED CODE 2 HEADER");
    EXPECT_EQ(result.line[3].bad, 1);
    EXPECT_EQ(text(result, 4), "");
    EXPECT_EQ(text(result, 5), "");
}

TEST(Refused, a_changed_card_record_is_code_4_or_6_never_valid)
{
    auto page = reference_page();
    page[SWITCH_SD_CARD_OFFSET + 0x34] ^= 1;
    EXPECT_EQ(evaluate(status_block(), page).reference_error, SWITCH_SD_REF_CRC);
    page = reference_page();
    page[SWITCH_SD_CARD_OFFSET + 0x80] ^= 1;
    EXPECT_EQ(evaluate(status_block(), page).reference_error, SWITCH_SD_REF_CRC);
}

TEST(Refused, an_all_zero_page_is_refused_and_shows_no_card)
{
    const std::array<uint8_t, 0x1000> zero = {};
    const auto result = evaluate(status_block(), zero);
    EXPECT_LE(1, result.reference_error);
    EXPECT_EQ(result.line[3].bad, 1);
    EXPECT_EQ(text(result, 4), "");
}

TEST(Status, a_missing_or_foreign_block_is_said_and_marked_bad)
{
    std::array<uint8_t, 0x100> none = {};
    auto result = evaluate(none, reference_page());
    EXPECT_EQ(result.status_ok, 0);
    EXPECT_EQ(result.line[0].bad, 1);
    EXPECT_EQ(text(result, 0), "SD STATUS BLOCK MISSING OR WRONG VERSION 00000000");
    EXPECT_EQ(text(result, 1), "");
}

TEST(Status, fewer_than_ten_flags_is_bad)
{
    const auto result = evaluate(status_block(9), reference_page());
    EXPECT_EQ(result.flags_set, 9u);
    EXPECT_EQ(result.line[0].bad, 1);
}

TEST(Status, a_flag_that_is_not_one_does_not_count)
{
    auto block = status_block();
    put_le32(&block[SWITCH_DIAG_SD_FLAGS_OFFSET + 8], 2);
    EXPECT_EQ(evaluate(block, reference_page()).flags_set, 9u);
}

TEST(Status, a_crc_that_differs_from_the_expected_value_is_bad)
{
    auto block = status_block();
    put_be32(&block[0x18], 0x630d1cb9);
    const auto result = evaluate(block, reference_page());
    EXPECT_EQ(result.line[2].bad, 1);
    EXPECT_EQ(result.line[1].bad, 0);
}

TEST(Names, the_error_codes_are_the_validators)
{
    EXPECT_EQ(std::string(switch_diag_sd_error_name(SWITCH_SD_REF_OK)), "OK");
    EXPECT_EQ(std::string(switch_diag_sd_error_name(SWITCH_SD_REF_CARD)), "CARD");
    EXPECT_EQ(std::string(switch_diag_sd_error_name(99)), "UNKNOWN");
}
