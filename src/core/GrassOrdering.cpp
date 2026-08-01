#include "core/GrassScene.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace neomdl {

std::vector<GrassGroup> engineGrassGroups(const GrassBin& bin) {
    std::size_t total = 0;
    for (const auto& triangle : bin.triangles) {
        if (total > bin.bladeCount || triangle.bladeCount > bin.bladeCount - total)
            throw std::invalid_argument("Grass sorting: triangle counts exceed bin");
        total += triangle.bladeCount;
    }
    if (total != bin.bladeCount)
        throw std::invalid_argument("Grass sorting: mismatched face/bin counts");
    if (!bin.lightmapped) return {};

    std::vector<std::size_t> order(bin.triangles.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](auto left, auto right) {
        return bin.triangles[left].part < bin.triangles[right].part;
    });

    std::vector<GrassGroup> result;
    std::size_t first = 0;
    for (std::size_t begin = 0; begin < order.size();) {
        const auto part = bin.triangles[order[begin]].part;
        auto end = begin;
        std::size_t count = 0;
        while (end < order.size() && bin.triangles[order[end]].part == part) {
            count += bin.triangles[order[end]].bladeCount;
            ++end;
        }
        // Geometry remains in admission order. Its original group boundary
        // also supplies the lightmap part consumed by the split-array draw.
        const auto& lightmap = bin.triangles[end - 1];
        result.push_back({first * 4u, count, part, lightmap.lightmap, lightmap.part});
        first += count;
        begin = end;
    }
    return result;
}

} // namespace neomdl
