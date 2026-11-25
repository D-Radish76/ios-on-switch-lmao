#include <stdint.h>
#include "t1t.h"

uint32_t switch_t1t_read32(uint32_t address);

/* Every T1-T device read goes through here, and the allowlist decides before the address is used. */
__attribute__((noinline)) uint32_t switch_t1t_access(unsigned reg)
{
    if (!switch_t1t_may_read(reg))
        return 0;
    const uint32_t address = switch_t1t_address(reg);
    return address ? switch_t1t_read32(address) : 0;
}
