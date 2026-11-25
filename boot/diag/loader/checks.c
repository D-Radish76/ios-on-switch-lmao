#include "checks.h"

static int overlaps(uint64_t start, uint64_t end, struct switch_diag_span other)
{
    return start < other.end && other.start < end;
}

static uint32_t be32(const volatile uint32_t *words, uint32_t offset)
{
    return __builtin_bswap32(words[offset / 4]);
}

int switch_diag_entry_state(uint64_t current_el, uint64_t sctlr, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t midr,
                            uint64_t cntfrq)
{
    if (current_el != 8 || (sctlr & 0x1005))
        return SWITCH_DIAG_ENTRY_EL;
    if (x1 || x2 || x3)
        return SWITCH_DIAG_ENTRY_ARGUMENTS;
    if (((midr >> 24) & 255) != 0x41 || ((midr >> 4) & 0xfff) != 0xd07)
        return SWITCH_DIAG_ENTRY_CPU;
    if (cntfrq != SWITCH_DIAG_CNTFRQ)
        return SWITCH_DIAG_ENTRY_TIMER;
    return 0;
}

int switch_diag_el1_state(const struct switch_diag_el1 *s)
{
    if (s->current_el != 4 || (s->sctlr & 0x1005) || (s->daif & 0x3c0) != 0x3c0 || s->cntfrq != SWITCH_DIAG_CNTFRQ ||
        s->hcr != 0x80000000 || s->cnthctl != 3 || s->cntvoff)
        return SWITCH_DIAG_EL1_STATE;
    return 0;
}

int switch_diag_fdt_pointer(uint64_t pointer, struct switch_diag_span image)
{
    if (pointer != SWITCH_DIAG_FDT_PA)
        return SWITCH_DIAG_FDT_ADDRESS;
    if (pointer & 7)
        return SWITCH_DIAG_FDT_ALIGNMENT;
    if (pointer < SWITCH_DIAG_DRAM_START || pointer + SWITCH_DIAG_FDT_BYTES > SWITCH_DIAG_DRAM_END)
        return SWITCH_DIAG_FDT_DRAM;
    if (overlaps(pointer, pointer + SWITCH_DIAG_FDT_BYTES, image))
        return SWITCH_DIAG_FDT_OVERLAPS_IMAGE;
    return 0;
}

int switch_diag_fdt_header(uint32_t magic, uint32_t total, uint32_t reserve, uint32_t structure)
{
    if (magic != 0xd00dfeed)
        return SWITCH_DIAG_FDT_MAGIC;
    if (total < 40 || total > SWITCH_DIAG_FDT_BYTES)
        return SWITCH_DIAG_FDT_TOTAL;
    if (reserve < 40 || reserve % 8 || structure % 4 || structure > total || structure < reserve ||
        structure - reserve < 16)
        return SWITCH_DIAG_FDT_OFFSETS;
    return 0;
}

int switch_diag_fdt_reservations(const volatile uint32_t *fdt, uint32_t reserve, uint32_t structure,
                                 struct switch_diag_span window, uint32_t *entries, int *covers)
{
    *entries = 0;
    *covers = 0;
    for (uint32_t at = reserve;; at += 16) {
        if (at > structure - 16)
            return SWITCH_DIAG_FDT_RESERVATION_END;
        const uint64_t base = (uint64_t)be32(fdt, at) << 32 | be32(fdt, at + 4);
        const uint64_t size = (uint64_t)be32(fdt, at + 8) << 32 | be32(fdt, at + 12);
        if (!base && !size)
            return 0;
        if (!size || size > UINT64_MAX - base)
            return SWITCH_DIAG_FDT_RESERVATION_SIZE;
        if (*entries == SWITCH_DIAG_RESERVATIONS_MAX)
            return SWITCH_DIAG_FDT_RESERVATION_COUNT;
        ++*entries;
        if (base <= window.start && base + size >= window.end)
            *covers = 1;
    }
}

int switch_diag_framebuffer(uint64_t base, uint64_t bytes, int reserved, struct switch_diag_span image)
{
    if (!bytes || (base & 0xfff) || bytes > UINT64_MAX - base)
        return SWITCH_DIAG_FB_GEOMETRY;
    if (base < SWITCH_DIAG_DRAM_START || base + bytes > SWITCH_DIAG_DRAM_END)
        return SWITCH_DIAG_FB_DRAM;
    if (overlaps(base, base + bytes, image))
        return SWITCH_DIAG_FB_OVERLAPS_IMAGE;
    if (!reserved)
        return SWITCH_DIAG_FB_NOT_RESERVED;
    return 0;
}

uint64_t switch_diag_wait_ticks(uint64_t frequency, unsigned seconds)
{
    if (frequency != SWITCH_DIAG_CNTFRQ || !seconds || seconds > 60)
        return 0;
    return frequency * seconds;
}

int switch_diag_before(uint64_t now, uint64_t deadline)
{
    return (int64_t)(now - deadline) < 0;
}
