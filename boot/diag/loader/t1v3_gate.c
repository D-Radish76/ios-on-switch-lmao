#include <stdint.h>
#include "t1v3.h"

uint32_t switch_t1v3_read32(uint32_t address);
void switch_t1v3_write32(uint32_t address, uint32_t value);

/* Every T1 v3 access goes through here, and the allowlist decides before the address is used. */
__attribute__((noinline)) uint32_t switch_t1v3_access(unsigned reg, int write, uint32_t value)
{
    if (write ? !switch_t1v3_may_write(reg, value) : !switch_t1v3_may_read(reg))
        return 0;
    const uint32_t address = switch_t1v3_address(reg);
    if (!address)
        return 0;
    if (write) {
        switch_t1v3_write32(address, value);
        return 0;
    }
    return switch_t1v3_read32(address);
}
