#include "report.h"

static void put(struct switch_diag_line *line, char c)
{
    if (line->length < SWITCH_DIAG_LINE_MAX)
        line->text[line->length++] = c;
    line->text[line->length] = 0;
}

void switch_diag_line_add(struct switch_diag_line *line, const char *text)
{
    for (; *text; ++text)
        put(line, *text);
}

void switch_diag_line_clip(struct switch_diag_line *line, unsigned length)
{
    if (line->length > length)
        line->length = length;
    line->text[line->length] = 0;
}

void switch_diag_line_start(struct switch_diag_line *line, const char *text)
{
    line->length = 0;
    line->text[0] = 0;
    switch_diag_line_add(line, text);
}

void switch_diag_line_hex(struct switch_diag_line *line, uint64_t value, unsigned digits)
{
    static const char alphabet[] = "0123456789ABCDEF";
    for (unsigned i = digits; i; --i)
        put(line, alphabet[(value >> ((i - 1) * 4)) & 15]);
}

void switch_diag_line_decimal(struct switch_diag_line *line, uint64_t value)
{
    char digits[20];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        put(line, digits[--count]);
}

void switch_diag_register_line(struct switch_diag_line *line, uint32_t address, uint32_t value, uint32_t expected)
{
    switch_diag_line_start(line, "");
    switch_diag_line_hex(line, address, 8);
    put(line, ' ');
    switch_diag_line_hex(line, value, 8);
    if (value == expected) {
        switch_diag_line_add(line, " SAME");
        return;
    }
    switch_diag_line_add(line, " DIFFERENT ");
    switch_diag_line_hex(line, expected, 8);
}
