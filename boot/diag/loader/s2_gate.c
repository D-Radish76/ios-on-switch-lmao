#include <stdint.h>
#include "s2.h"

uint32_t switch_s2_read32(uint32_t address);

/* Every S2 device read goes through here, and the allowlist decides before the address is used. */
__attribute__((noinline)) uint32_t switch_s2_access(unsigned reg)
{
    if (!switch_s2_may_read(reg))
        return 0;
    const uint32_t address = switch_s2_address(reg);
    return address ? switch_s2_read32(address) : 0;
}
