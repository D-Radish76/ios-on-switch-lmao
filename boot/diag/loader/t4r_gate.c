#include <stdint.h>
#include "t4r.h"

uint32_t switch_t4r_read32(uint32_t address);

/* Every T4-R device read goes through here, and the allowlist decides before the address is used. */
__attribute__((noinline)) uint32_t switch_t4r_access(unsigned reg)
{
    if (!switch_t4r_may_read(reg, T4R_WIDTH))
        return 0;
    const uint32_t address = switch_t4r_address(reg);
    if (!address)
        return 0;
    return switch_t4r_read32(address);
}
