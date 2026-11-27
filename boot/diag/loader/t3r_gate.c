#include <stdint.h>
#include "t3r.h"

uint32_t switch_t3r_read32(uint32_t address);

/* Every T3-R device read goes through here, and the allowlist decides before the address is used. */
__attribute__((noinline)) uint32_t switch_t3r_access(unsigned reg)
{
    if (!switch_t3r_may_read(reg, T3R_WIDTH))
        return 0;
    const uint32_t address = switch_t3r_address(reg);
    if (!address)
        return 0;
    return switch_t3r_read32(address);
}
