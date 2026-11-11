// Copyright 2026 PyMatching Contributors
// Licensed under the Apache License, Version 2.0.
#ifndef PYMATCHING_STRUCTURED_CIRCUIT_STENCIL_H
#define PYMATCHING_STRUCTURED_CIRCUIT_STENCIL_H

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace pm::structured {

// This switch affects only the structured backend. The generic backend and
// shared geometric direction IDs always retain their sixteen-direction ABI.
#ifndef PM_STRUCTURED_STORAGE_DIRECTIONS
#define PM_STRUCTURED_STORAGE_DIRECTIONS 12
#endif
static_assert(PM_STRUCTURED_STORAGE_DIRECTIONS == 12 || PM_STRUCTURED_STORAGE_DIRECTIONS == 16);
inline constexpr size_t STRUCTURED_STORAGE_DIRECTIONS = PM_STRUCTURED_STORAGE_DIRECTIONS;

// Each component visits twelve compile-time geometric directions. X/Z labels
// describe the rotated circuit coordinate convention, not component ordering.
enum class CircuitStencilKind : uint8_t { x, z, unrotated };
inline constexpr size_t CIRCUIT_SCAN_DIRECTIONS = 12;

template <size_t... D>
constexpr uint16_t circuit_direction_mask(std::index_sequence<D...>) {
    static_assert(sizeof...(D) == CIRCUIT_SCAN_DIRECTIONS);
    return ((uint16_t{1} << D) | ...);
}

template <CircuitStencilKind Kind>
struct CircuitStencil;

template <>
struct CircuitStencil<CircuitStencilKind::x> {
    using Directions = std::index_sequence<0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13>;
    static constexpr uint16_t mask = circuit_direction_mask(Directions{});
    static constexpr std::array<size_t, CIRCUIT_SCAN_DIRECTIONS> boundary_directions{
        2, 3, 4, 5, 0, 1, 6, 7, 8, 9, 12, 13};
};

template <>
struct CircuitStencil<CircuitStencilKind::z> {
    using Directions = std::index_sequence<0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 14, 15>;
    static constexpr uint16_t mask = circuit_direction_mask(Directions{});
    static constexpr std::array<size_t, CIRCUIT_SCAN_DIRECTIONS> boundary_directions{
        2, 3, 4, 5, 0, 1, 6, 7, 10, 11, 14, 15};
};

template <>
struct CircuitStencil<CircuitStencilKind::unrotated> {
    using Directions = std::make_index_sequence<CIRCUIT_SCAN_DIRECTIONS>;
    static constexpr uint16_t mask = circuit_direction_mask(Directions{});
    static constexpr std::array<size_t, CIRCUIT_SCAN_DIRECTIONS> boundary_directions{
        2, 3, 4, 5, 0, 1, 6, 7, 8, 9, 10, 11};
};

template <typename Stencil>
struct CircuitStorage {
    static constexpr size_t slot_count = STRUCTURED_STORAGE_DIRECTIONS;
    static constexpr auto directions = []<size_t... D>(std::index_sequence<D...>) {
        return std::array<size_t, sizeof...(D)>{D...};
    }(typename Stencil::Directions{});
    static constexpr auto global_to_slot = [] {
        std::array<size_t, 16> result;
        result.fill(SIZE_MAX);
        for (size_t i = 0; i < directions.size(); i++)
            result[directions[i]] = slot_count == 12 ? i : directions[i];
        return result;
    }();
    static constexpr auto slot_to_global = [] {
        std::array<size_t, slot_count> result;
        result.fill(SIZE_MAX);
        for (size_t d : directions)
            result[global_to_slot[d]] = d;
        return result;
    }();
    static constexpr auto slots = [] {
        std::array<size_t, CIRCUIT_SCAN_DIRECTIONS> result;
        for (size_t i = 0; i < result.size(); i++)
            result[i] = global_to_slot[directions[i]];
        return result;
    }();
    static constexpr uint16_t mask = [] {
        uint16_t result = 0;
        for (size_t s : slots)
            result |= uint16_t{1} << s;
        return result;
    }();

    template <size_t D>
    static constexpr size_t slot = global_to_slot[D];
    template <size_t S>
    static constexpr size_t direction = slot_to_global[S];

    // Ascending geometric IDs preserve first-minimum ties. Packing complete
    // opposite-direction pairs preserves the edge reversal operation s ^ 1.
    static_assert([] {
        for (size_t i = 0; i < directions.size(); i++) {
            size_t d = directions[i], s = slots[i];
            if (slot_to_global[s ^ 1] != (d ^ 1) ||
                (i && (directions[i - 1] >= d || slots[i - 1] >= s)))
                return false;
        }
        return std::popcount(mask) == CIRCUIT_SCAN_DIRECTIONS;
    }());
};

// Construction only. Reduced graphs may fit more than one stencil; the fixed
// order is safe because boundary edges are also restricted to the chosen set.
inline CircuitStencilKind circuit_stencil_for_mask(uint16_t mask) {
    if (!(mask & ~CircuitStencil<CircuitStencilKind::x>::mask))
        return CircuitStencilKind::x;
    if (!(mask & ~CircuitStencil<CircuitStencilKind::z>::mask))
        return CircuitStencilKind::z;
    if (!(mask & ~CircuitStencil<CircuitStencilKind::unrotated>::mask))
        return CircuitStencilKind::unrotated;
    throw std::invalid_argument("Component edges do not fit a supported 12-direction circuit stencil.");
}

}  // namespace pm::structured
#endif
