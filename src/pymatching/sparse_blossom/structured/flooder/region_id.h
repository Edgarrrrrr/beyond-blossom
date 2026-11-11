// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#ifndef PYMATCHING_STRUCTURED_REGION_ID_H
#define PYMATCHING_STRUCTURED_REGION_ID_H
#include <cstdint>
namespace pm::structured {
using RegionId = uint32_t;
using RegionGeneration = uint32_t;
inline constexpr RegionId NO_REGION = 0;
}  // namespace pm::structured
#endif
