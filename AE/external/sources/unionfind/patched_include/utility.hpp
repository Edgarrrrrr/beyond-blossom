#pragma once
#include <algorithm>
#include <climits>
#include <cstdint>
#include <functional>
#include <ostream>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef NDEBUG
#define DEBUG
#endif

namespace UnionFindCPP
{

enum class ErrorType
{
	X,
	Z
};

struct Edge
{
	uint32_t u;
	uint32_t v;

	Edge(uint32_t ul, uint32_t vl)
	{
		u = std::min(ul, vl);
		v = std::max(ul, vl);
	}

	inline auto operator==(const Edge& rhs) const -> bool
	{
		return (u == rhs.u) && (v == rhs.v);
	}
};

void to_json(nlohmann::json& j, const Edge& e);
void from_json(const nlohmann::json& j, Edge& e);
auto operator<<(std::ostream& os, const UnionFindCPP::Edge& e) -> std::ostream&;
} // namespace UnionFindCPP

template<> struct std::hash<UnionFindCPP::Edge>
{
	auto operator()(const UnionFindCPP::Edge& e) const noexcept -> std::size_t
	{
        // Sequential detector IDs make the original XOR hash occupy only
        // a small contiguous range. robin-map repeatedly grows when probe
        // distance exceeds 4096, even though growth cannot disperse that range.
        // Mix both full endpoint IDs before robin-map selects its bucket.
        uint64_t x = (static_cast<uint64_t>(e.u) << 32U) | e.v;
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
        return static_cast<std::size_t>(x ^ (x >> 31U));
	}
};
