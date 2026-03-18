#pragma once

#include <cstdint>

namespace fre {

// ─── Combinatorial hash seeds ────────────────────────────────────────────────
//
// Shared by TenantRouter (cell-level) and FleetRouter (instance-level) to
// guarantee bit-identical shuffle shard assignments at every layer.
// Based on AWS shuffle sharding (Vogels 2014).
//
// IMPORTANT: Never change these values — doing so changes all tenant assignments
// across both layers simultaneously and will cause misrouting.

constexpr uint32_t k_hash_seeds[] = {
    0x9e3779b9u, 0x6c62272eu, 0x94d049bbu, 0xe9546b25u,
    0x12e15e35u, 0x3b1d8f2bu, 0x7c9e4ab3u, 0x4f5a1c9fu,
};

}  // namespace fre
