#include "live_shared.h"

int main(void)
{
    HznLiveShared map = {0};
    if (!hzn_live_shared_map(&map, UINT64_C(0x7600203000), 0x40000))
        return 1;
    *(volatile uint8_t *)map.writer = 0xa7;
    const bool reflected = *(const volatile uint8_t *)(uintptr_t)map.address == 0xa7;
    return !hzn_live_shared_unmap(&map) || !reflected;
}
