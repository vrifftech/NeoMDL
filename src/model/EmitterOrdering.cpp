#include <model/Emitter.hpp>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace neomdl::model {
namespace {
struct SortKey {
    bool lateCategory{};
    double distance{};
    std::uint64_t owner{};
    std::int16_t renderOrder{};
    std::uint64_t identity{};
};
double cameraDistance(Vec3 camera,Vec3 position) noexcept {
    for(const auto value:{camera.x,camera.y,camera.z,position.x,position.y,position.z})
        if(!std::isfinite(value))return -1.0;
    return std::hypot(double(camera.x)-double(position.x),
        double(camera.y)-double(position.y),double(camera.z)-double(position.z));
}
SortKey sortKey(const EmitterBucketEntry& entry,Vec3 camera) noexcept {
    return {entry.category==2,cameraDistance(camera,
        entry.hasOwner?entry.ownerPosition:entry.emitterPosition),
        entry.hasOwner?entry.ownerIdentity:0u,
        entry.hasOwner?entry.renderOrder:std::int16_t{},entry.identity};
}
bool before(const SortKey& a,const SortKey& b) noexcept {
    if(a.lateCategory!=b.lateCategory)return !a.lateCategory;
    if(a.distance!=b.distance)return a.distance>b.distance;
    if(a.owner!=b.owner)return a.owner<b.owner;
    if(a.renderOrder!=b.renderOrder)return a.renderOrder<b.renderOrder;
    return a.identity<b.identity;
}
std::vector<std::size_t> ordinals(const std::vector<EmitterBucketEntry>& entries,Vec3 camera) {
    std::vector<SortKey> keys;keys.reserve(entries.size());
    for(const auto& entry:entries)keys.push_back(sortKey(entry,camera));
    std::vector<std::size_t> order(entries.size());
    std::iota(order.begin(),order.end(),0u);
    std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return before(keys[a],keys[b]);});
    return order;
}
}
std::vector<std::size_t> engineEmitterBucketDrawOrder(
    const std::vector<EmitterBucketEntry>& entries,Vec3 camera) {
    auto order=ordinals(entries,camera);
    for(auto& index:order)index=entries[index].emitterIndex;
    return order;
}
void sortK2EmitterBucket(std::vector<EmitterBucketEntry>& entries,Vec3 camera) {
    const auto order=ordinals(entries,camera);
    std::vector<EmitterBucketEntry> sorted;sorted.reserve(entries.size());
    for(const auto index:order)sorted.push_back(entries[index]);
    entries=std::move(sorted);
}
} // namespace neomdl::model
