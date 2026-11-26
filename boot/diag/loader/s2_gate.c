#include <stdint.h>
#include "s2.h"

uint32_t switch_s2_read32(uint32_t address);
uint32_t switch_s2_read16(uint32_t address);

/* Every S2 device read goes through here, and the allowlist decides on the register and its width before the address is used. */
__attribute__((noinline)) uint32_t switch_s2_access(unsigned reg, unsigned width)
{
    if (!switch_s2_may_read(reg, width))
        return 0;
    const uint32_t address = switch_s2_address(reg);
    if (!address)
        return 0;
    return width == S2_WIDTH_16 ? switch_s2_read16(address) : switch_s2_read32(address);
}
