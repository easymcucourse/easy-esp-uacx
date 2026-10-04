#include "uac2_internal.h"

uint32_t uac2_packet_bytes(uint32_t rate, uint8_t channels, uint8_t subslot, uint32_t intervals_per_sec)
{
    if (!intervals_per_sec) return 0;
    uint64_t bps = (uint64_t)rate * channels * subslot;
    return (uint32_t)((bps + intervals_per_sec - 1) / intervals_per_sec);
}

uint32_t uac2_dsd_dop_rate(uac2_dsd_rate_t r) { return (uint32_t)r * 44100u / 16u; }
uint32_t uac2_dsd_native_rate(uac2_dsd_rate_t r) { return (uint32_t)r * 44100u / 32u; }
