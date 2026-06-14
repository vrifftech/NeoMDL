#include <model/Trigger.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace neomdl::model {
namespace {

Vec3 add(Vec3 left, Vec3 right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}
Vec3 subtract(Vec3 left, Vec3 right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}
Vec3 multiply(Vec3 value, float scalar) noexcept {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}
float dot(Vec3 left, Vec3 right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}
Vec3 cross(Vec3 left, Vec3 right) noexcept {
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}
float lengthSquared(Vec3 value) noexcept { return dot(value, value); }

void extend(Bounds& bounds, Vec3 point) noexcept {
    if (!bounds.valid) {
        bounds.minimum = bounds.maximum = point;
        bounds.valid = true;
        return;
    }
    bounds.minimum.x = std::min(bounds.minimum.x, point.x);
    bounds.minimum.y = std::min(bounds.minimum.y, point.y);
    bounds.minimum.z = std::min(bounds.minimum.z, point.z);
    bounds.maximum.x = std::max(bounds.maximum.x, point.x);
    bounds.maximum.y = std::max(bounds.maximum.y, point.y);
    bounds.maximum.z = std::max(bounds.maximum.z, point.z);
}

Bounds boundsOf(const std::vector<Vec3>& points) noexcept {
    Bounds result;
    for (const auto point : points) extend(result, point);
    return result;
}

void decompose(const std::vector<Vec3>& points,
               std::vector<std::size_t> indices,
               std::vector<Vec3>& triangles) {
    // K1 Desktop 0x371cc6 / K2 Desktop 0x0b3612. The inserted interior vertex
    // participates in the triangle emitted before the recursive sub-list.
    while (indices.size() > 2u) {
        float minimumX = 1000000.0f;
        std::size_t current = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < indices.size(); ++i) {
            if (points[indices[i]].x < minimumX) {
                minimumX = points[indices[i]].x;
                current = i;
            }
        }
        auto first = indices[(current + indices.size() - 1u) % indices.size()];
        const auto center = indices[current];
        const auto last = indices[(current + 1u) % indices.size()];
        std::array<Vec3, 3> candidate{{points[first], points[center], points[last]}};
        for (auto& p : candidate) p.z = 0.0f;
        std::optional<std::size_t> interior;
        float interiorMinimumX = 1000000.0f;
        for (const auto index : indices) {
            if (index == first || index == center || index == last) continue;
            const auto point = points[index];
            if (hitTestTriggerTriangle(candidate, {point.x, point.y, 1000.0f},
                    {point.x, point.y, -1000.0f}) && point.x < interiorMinimumX) {
                interiorMinimumX = point.x;
                interior = index;
            }
        }
        std::optional<std::size_t> split;
        if (interior) {
            first = *interior;
            if (current == indices.size() - 1u) {
                indices.push_back(*interior);
            } else {
                indices.insert(indices.begin() + static_cast<std::ptrdiff_t>(current + 1u),
                               *interior);
                split = *interior;
            }
        } else {
            indices.erase(indices.begin() + static_cast<std::ptrdiff_t>(current));
        }
        triangles.push_back(points[first]);
        triangles.push_back(points[center]);
        triangles.push_back(points[last]);
        if (split) {
            const auto firstCopy = std::find(indices.begin(), indices.end(), *split);
            const auto position = static_cast<std::size_t>(firstCopy - indices.begin()) + 1u;
            std::vector<std::size_t> subList;
            std::size_t index;
            do {
                index = indices[position];
                subList.push_back(index);
                indices.erase(indices.begin() + static_cast<std::ptrdiff_t>(position));
            } while (index != *split);
            decompose(points, std::move(subList), triangles);
        }
    }
}

bool segmentIntersectsBounds(Vec3 start, Vec3 end,
                             const Bounds& bounds) noexcept {
    // BoxIntersect, K1 Desktop 0x3a56ec: region rejection, then X/Y/Z faces.
    const float from[3]{start.x, start.y, start.z};
    const float to[3]{end.x, end.y, end.z};
    const float lower[3]{bounds.minimum.x, bounds.minimum.y, bounds.minimum.z};
    const float upper[3]{bounds.maximum.x, bounds.maximum.y, bounds.maximum.z};
    unsigned startRegion = 0u, endRegion = 0u;
    for (unsigned axis = 0; axis < 3; ++axis) {
        const auto low = 1u << (5u - axis * 2u), high = low >> 1u;
        if (from[axis] < lower[axis]) startRegion |= low;
        else if (from[axis] > upper[axis]) startRegion |= high;
        if (to[axis] < lower[axis]) endRegion |= low;
        else if (to[axis] > upper[axis]) endRegion |= high;
    }
    if (!startRegion) return true;
    if (startRegion & endRegion) return false;
    for (unsigned axis = 0; axis < 3; ++axis) {
        const auto low = 1u << (5u - axis * 2u), high = low >> 1u;
        if (!(startRegion & (low | high))) continue;
        const auto face = startRegion & low ? lower[axis] : upper[axis];
        const float fraction = (face - from[axis]) / (to[axis] - from[axis]);
        bool inside = true;
        for (unsigned other = 0; other < 3; ++other) {
            if (other == axis) continue;
            const float point = from[other] * (1.0f - fraction) + to[other] * fraction;
            if (!(point >= lower[other] && point <= upper[other])) inside = false;
        }
        if (inside) return true;
    }
    return false;
}

} // namespace

std::optional<TriggerHit> hitTestTriggerTriangle(
    const std::array<Vec3, 3>& triangle, Vec3 start, Vec3 end) noexcept {
    // polyhit's three-vertex case: K1 Desktop 0x3a67b8, K2 Desktop 0x315237.
    auto normal = cross(subtract(triangle[1], triangle[0]),
                        subtract(triangle[2], triangle[1]));
    const float length = std::sqrt(lengthSquared(normal));
    if (length < 1.0e-8f) return std::nullopt;
    normal = multiply(normal, 1.0f / length);
    const float plane = -dot(normal, triangle[0]);
    const float from = dot(normal, start) + plane;
    const float to = dot(normal, end) + plane;
    if (from < 0.0f || to > 0.0f || from == to) return std::nullopt;
    const float fraction = from / (from - to);
    const auto point = add(multiply(start, 1.0f - fraction), multiply(end, fraction));
    for (std::size_t edge = 0; edge < triangle.size(); ++edge) {
        const auto side = dot(normal, cross(
            subtract(triangle[(edge + 1u) % triangle.size()], triangle[edge]),
            subtract(point, triangle[edge])));
        if (!(side >= 0.0f)) return std::nullopt;
    }
    return TriggerHit{point, normal, 0u, 0u, 2.0f, fraction};
}

TriggerVolume makeTriggerVolume(std::vector<Vec3> footprint,
                                std::array<float, 4> color,
                                std::uintptr_t owner) {
    TriggerVolume result;
    result.footprint = std::move(footprint);
    result.color = color;
    result.owner = owner;
    // AurPartTriggerCreate seeds this box, unlike VectorMin/Max(List).
    result.bounds.minimum = {100000.0f, 100000.0f, 100000.0f};
    result.bounds.maximum = {-100000.0f, -100000.0f, -100000.0f};
    result.bounds.valid = true;
    for (const auto point : result.footprint) extend(result.bounds, point);
    std::vector<std::size_t> indices(result.footprint.size());
    std::iota(indices.begin(), indices.end(), 0u);
    decompose(result.footprint, std::move(indices), result.triangles);
    result.valid = true;
    return result;
}

void setTriggerHighlighted(TriggerVolume& volume, bool highlighted,
                             float height, bool group) {
    volume.highlighted = highlighted;
    volume.highlightGroup = group;
    volume.highlightHalfHeight = std::fabs(height);
    if (volume.footprint.size() < 2u || !highlighted) return;

    volume.sideStrip.clear();
    volume.top.clear();
    volume.bottom.clear();
    volume.sideStrip.reserve(volume.footprint.size() * 2u + 2u);
    volume.top.reserve(volume.footprint.size());
    volume.bottom.reserve(volume.footprint.size());
    for (const auto point : volume.footprint) {
        const Vec3 top{point.x, point.y, point.z + volume.highlightHalfHeight};
        const Vec3 bottom{point.x, point.y,
                          top.z + volume.highlightHalfHeight * -2.0f};
        volume.sideStrip.push_back(top);
        volume.top.push_back(top);
        volume.sideStrip.push_back(bottom);
    }
    // SetHighlighted emits the bottom polygon from the final footprint point
    // back to the first so the two caps have opposite winding.
    for (auto position = volume.footprint.rbegin();
         position != volume.footprint.rend(); ++position) {
        volume.bottom.push_back(
            {position->x, position->y,
             position->z - volume.highlightHalfHeight});
    }
    volume.sideStrip.push_back(volume.sideStrip[0]);
    volume.sideStrip.push_back(volume.sideStrip[1]);
    volume.bounds = boundsOf(volume.sideStrip);
}

void setTriggerHighlightParameters(
    TriggerVolume& volume, const TriggerHighlightParameters& parameters) {
    volume.highlight = parameters;
}

void setTriggerHighlightColor(TriggerVolume& volume, Vec3 color,
                                float alpha) {
    volume.highlight.color = color;
    volume.highlight.alpha = alpha;
}

float triggerEffectiveBaseAlpha(const TriggerVolume& volume,
                                  float globalAlpha) noexcept {
    return volume.color[3] + globalAlpha;
}

float triggerHighlightAlpha(const TriggerVolume& volume,
                              float scenePhase) noexcept {
    if (!volume.highlight.pulse) return volume.highlight.alpha;
    return volume.highlight.alpha * 0.5f *
        (std::sin(volume.highlight.pulseRate * scenePhase) + 1.0f);
}

std::optional<TriggerHit> hitTestTrigger(
    const TriggerVolume& volume, const Vec3& start, const Vec3& end,
    const TriggerHitOptions& options) {
    if (!options.geometryEnabled || !volume.valid ||
        !segmentIntersectsBounds(start, end, volume.bounds)) {
        return std::nullopt;
    }
    for (std::size_t offset = 0u; offset + 2u < volume.triangles.size();
         offset += 3u) {
        const std::array<Vec3, 3> triangle{{volume.triangles[offset],
            volume.triangles[offset + 1u], volume.triangles[offset + 2u]}};
        if (auto hit = hitTestTriggerTriangle(triangle, start, end)) {
            hit->triangleIndex = offset / 3u;
            hit->owner = volume.owner;
            return hit;
        }
    }
    return std::nullopt;
}

bool triggerOutsideFrustum(const TriggerVolume& volume,
                             const std::vector<TriggerPlane>& planes,
                             std::size_t& cachedPlane) noexcept {
    if (!volume.bounds.valid || planes.empty()) return false;
    cachedPlane %= planes.size();
    for (std::size_t offset = 0u; offset < planes.size(); ++offset) {
        const auto index = (cachedPlane + offset) % planes.size();
        const auto& plane = planes[index];
        const Vec3 corner{
            plane.normal.x > 0.0f ? volume.bounds.minimum.x
                                  : volume.bounds.maximum.x,
            plane.normal.y > 0.0f ? volume.bounds.minimum.y
                                  : volume.bounds.maximum.y,
            plane.normal.z > 0.0f ? volume.bounds.minimum.z
                                  : volume.bounds.maximum.z};
        const auto distance = dot(plane.normal, corner) + plane.distance;
        if (distance > 0.0f) {
            cachedPlane = offset;
            return true;
        }
    }
    return false;
}

} // namespace neomdl::model
