// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#ifndef BB_STRUCTURED_REGION_ID_H
#define BB_STRUCTURED_REGION_ID_H
#include <cstdint>
namespace beyond_blossom::structured {
using RegionId = uint32_t;
using RegionGeneration = uint32_t;
inline constexpr RegionId NO_REGION = 0;
}  // namespace beyond_blossom::structured
#endif
