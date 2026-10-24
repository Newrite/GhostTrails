#include "GhostTrailsAPI.h"
#include <cstddef>
#include <iostream>
#include <type_traits>

import GhostTrails.ConsumerExample;

// These are the released ABI layout, not compiler-generated BMIs or STL types.
static_assert(std::is_standard_layout_v<GhostTrailsAPI::Appearance>);
static_assert(std::is_trivially_copyable_v<GhostTrailsAPI::SpawnOptions>);
static_assert(sizeof(GhostTrailsAPI::Appearance) == 24);
static_assert(sizeof(GhostTrailsAPI::SpawnOptions) == 44);
static_assert(sizeof(GhostTrailsAPI::Status) == 24);
static_assert(offsetof(GhostTrailsAPI::SpawnOptions, spawnWindow) == 40);

int main()
{
    const auto absent = GhostTrailsExample::Spawn(0);
    if (absent.result != GhostTrailsAPI::Result::NotReady || absent.id != 0) return 1;
    std::cout << "Independent SDK module imports without CommonLib; absent DLL handled\n";
    return 0;
}
