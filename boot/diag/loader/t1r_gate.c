#include <stdint.h>
#include "t1r.h"

uint32_t switch_t1r_read32(uint32_t address);

/* Every T1-R device read goes through here, and the allowlist decides before the address is used. */
__attribute__((noinline)) uint32_t switch_t1r_access(unsigned reg)
{
    if (!switch_t1r_may_read(reg))
        return 0;
    const uint32_t address = switch_t1r_address(reg);
    return address ? switch_t1r_read32(address) : 0;
}
