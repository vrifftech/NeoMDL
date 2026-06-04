#include <model/SceneOrdering.hpp>
#include <model/Model.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <utility>

namespace neomdl::model {
namespace {




constexpr std::array<K2SceneStage, 18> kK1DesktopStages{{
    K2SceneStage::FogSetup,
    K2SceneStage::StaticMeshGeometry,
    K2SceneStage::StaticTriangleBins,
    K2SceneStage::TriggerGeometry,
    K2SceneStage::TriggerHighlightBeforeDynamic,
    K2SceneStage::Shadows,
    K2SceneStage::DynamicGobs,
    K2SceneStage::PersonalSpace,
    K2SceneStage::Paths,
    K2SceneStage::TriggerHighlightAfterDynamic,
    K2SceneStage::Emitters,
    K2SceneStage::Lightsabers,
    K2SceneStage::DrawableSquares,
    K2SceneStage::MotionBlur,
    K2SceneStage::Distortion,
    K2SceneStage::FramebufferEffects,
    K2SceneStage::LensFlares,
    K2SceneStage::FramebufferEffects2,
}};

// KOTOR2sub desktop: distortion and the framebuffer modification pass occur
// after motion blur; drawable squares are a separate stage before motion blur.
constexpr std::array<K2SceneStage, 19> kDesktopStages{{
    K2SceneStage::FogSetup,
    K2SceneStage::StaticMeshGeometry,
    K2SceneStage::StaticTriangleBins,
    K2SceneStage::TriggerGeometry,
    K2SceneStage::TriggerHighlightBeforeDynamic,
    K2SceneStage::Shadows,
    K2SceneStage::DynamicGobs,
    K2SceneStage::HologramGobs,
    K2SceneStage::PersonalSpace,
    K2SceneStage::Paths,
    K2SceneStage::TriggerHighlightAfterDynamic,
    K2SceneStage::Emitters,
    K2SceneStage::Lightsabers,
    K2SceneStage::DrawableSquares,
    K2SceneStage::MotionBlur,
    K2SceneStage::Distortion,
    K2SceneStage::FramebufferEffects,
    K2SceneStage::LensFlares,
    K2SceneStage::FramebufferEffects2,
}};

bool classified(std::uint16_t value, std::uint16_t bit) noexcept {
    return (value & bit) != 0u;
}

struct TextureBucket {
    std::uint64_t identity{};
    std::uint64_t order{};
    std::size_t firstOrdinal{};
    std::vector<std::size_t> sourceIndices;
};

int compareGobClassification(GameVersion game,
                             const GobBucketEntry& left,
                             const GobBucketEntry& right,
                             bool newSorting,
                             bool forceSight) noexcept {
    const auto compareBit = [&](std::uint16_t bit, int leftSetResult) {
        const bool leftSet = classified(left.modelClassification, bit);
        const bool rightSet = classified(right.modelClassification, bit);
        return leftSet == rightSet ? 0 : leftSet ? leftSetResult : -leftSetResult;
    };
    if (const auto result = compareBit(0x0001u, -1)) return result;
    if (newSorting && left.hasTransparency != right.hasTransparency)
        return left.hasTransparency ? -1 : 1;

    if (game == GameVersion::Kotor1) {


        if (const auto result = compareBit(0x0004u, 1)) return result;
        if (const auto result = compareBit(0x0010u, -1)) return result;
        return compareBit(0x0008u, -1);
    }
    if (const auto result = compareBit(kK2ModelDoor, forceSight ? -1 : 1))
        return result;
    return compareBit(kK2ModelCharacter, forceSight ? 1 : -1);
}

int descendingFloatOrder(float left, float right) noexcept {
    // All NaNs form one final equivalence class; infinities retain their
    // numeric order and signed zeros compare equally.
    const bool leftNaN = std::isnan(left), rightNaN = std::isnan(right);
    if (leftNaN != rightNaN) return leftNaN ? 1 : -1;
    if (left > right) return -1;
    if (left < right) return 1;
    return 0;
}

bool finalGobBefore(GameVersion game,
                    const GobBucketEntry& left,
                    const GobBucketEntry& right,
                    bool newSorting,
                    bool forceSight) noexcept {
    // This is the visible draw order of the reverse bucket traversal.
    if (const auto result = descendingFloatOrder(left.alpha, right.alpha))
        return result < 0;

    if (const auto result = compareGobClassification(game, left, right,
                                                      newSorting, forceSight))
        return result > 0;

    if (const auto result = descendingFloatOrder(left.cameraDistance, right.cameraDistance))
        return result < 0;

    if (left.insertionOrder != right.insertionOrder)
        return left.insertionOrder < right.insertionOrder;
    if (left.ownerIdentity != right.ownerIdentity)
        return left.ownerIdentity < right.ownerIdentity;
    return left.sourceIndex < right.sourceIndex;
}
} // namespace

K2SceneStageSequence k2SceneStageOrder() noexcept {
    return {kDesktopStages.data(), kDesktopStages.size()};
}

EngineSceneStageSequence engineSceneStageOrder(GameVersion game) noexcept {
    if (game == GameVersion::Kotor1) {
        return {kK1DesktopStages.data(), kK1DesktopStages.size()};
    }
    return k2SceneStageOrder();
}

const char* k2SceneStageName(K2SceneStage stage) noexcept {
    switch (stage) {
    case K2SceneStage::FogSetup: return "DoFog";
    case K2SceneStage::StaticMeshGeometry: return "DoMeshBuckets";
    case K2SceneStage::StaticTriangleBins: return "DoBinBucket";
    case K2SceneStage::TriggerGeometry: return "DoTriggerBucket";
    case K2SceneStage::TriggerHighlightBeforeDynamic: return "DoHighlightTriggers(false)";
    case K2SceneStage::Shadows: return "RenderShadows";
    case K2SceneStage::DynamicGobs: return "DoGobBuckets";
    case K2SceneStage::HologramGobs: return "DoHologramGobBuckets";
    case K2SceneStage::PersonalSpace: return "RenderPersonalSpace";
    case K2SceneStage::Paths: return "RenderPaths";
    case K2SceneStage::TriggerHighlightAfterDynamic: return "DoHighlightTriggers(true)";
    case K2SceneStage::Emitters: return "DoEmitterBucket";
    case K2SceneStage::Lightsabers: return "DoLightSaberBucket";
    case K2SceneStage::DrawableSquares: return "GLRender::RenderDrawableSquares";
    case K2SceneStage::MotionBlur: return "RenderMotionBlur / DoBlurBuckets";
    case K2SceneStage::Distortion: return "DoDistortionBuckets";
    case K2SceneStage::FramebufferEffects: return "GLRender::FrameBufferModifications";
    case K2SceneStage::LensFlares: return "RenderLensFlares";
    case K2SceneStage::FramebufferEffects2: return "GLRender::FrameBufferModifications2";
    }
    return "Unknown K2 scene stage";
}

std::vector<std::size_t> staticMeshBucketOrder(
    const std::vector<StaticMeshBucketEntry>& entries) {
    std::vector<std::size_t> result;
    result.reserve(entries.size());
    std::vector<std::size_t> textureZero;
    std::vector<TextureBucket> buckets;
    std::unordered_map<std::uint64_t, std::size_t> bucketByIdentity;

    for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
        const auto& entry = entries[ordinal];
        if (entry.background) {
            result.push_back(entry.sourceIndex);
            continue;
        }
        if (entry.textureIdentity == 0u) {
            textureZero.push_back(entry.sourceIndex);
            continue;
        }
        const auto found = bucketByIdentity.find(entry.textureIdentity);
        if (found == bucketByIdentity.end()) {
            const auto index = buckets.size();
            bucketByIdentity.emplace(entry.textureIdentity, index);
            TextureBucket bucket;
            bucket.identity = entry.textureIdentity;
            bucket.order = entry.textureOrder;
            bucket.firstOrdinal = ordinal;
            bucket.sourceIndices.push_back(entry.sourceIndex);
            buckets.push_back(std::move(bucket));
        } else {
            auto& bucket = buckets[found->second];
            bucket.sourceIndices.push_back(entry.sourceIndex);
            if (bucket.order == 0u ||
                (entry.textureOrder != 0u && entry.textureOrder < bucket.order)) {
                bucket.order = entry.textureOrder;
            }
        }
    }

    std::stable_sort(buckets.begin(), buckets.end(),
        [](const TextureBucket& left, const TextureBucket& right) {
            const auto unknown = std::numeric_limits<std::uint64_t>::max();
            const auto leftOrder = left.order == 0u ? unknown : left.order;
            const auto rightOrder = right.order == 0u ? unknown : right.order;
            if (leftOrder != rightOrder) return leftOrder < rightOrder;
            if (left.firstOrdinal != right.firstOrdinal)
                return left.firstOrdinal < right.firstOrdinal;
            return left.identity < right.identity;
        });
    for (const auto& bucket : buckets)
        result.insert(result.end(), bucket.sourceIndices.begin(), bucket.sourceIndices.end());
    result.insert(result.end(), textureZero.begin(), textureZero.end());
    return result;
}

std::vector<std::size_t> gobBucketDrawOrder(
    GameVersion game,
    const std::vector<GobBucketEntry>& entries,
    bool newSorting,
    bool forceSight,
    bool partitionForceSight) {
    forceSight = game == GameVersion::Kotor2 && forceSight;
    std::vector<std::size_t> indices(entries.size());
    std::iota(indices.begin(), indices.end(), 0u);
    std::stable_sort(indices.begin(), indices.end(),
        [&](std::size_t left, std::size_t right) {
            return finalGobBefore(game, entries[left], entries[right],
                                  newSorting, forceSight);
        });

    if(!partitionForceSight) {
        for(auto& index:indices)index=entries[index].sourceIndex;
        return indices;
    }
    std::vector<std::size_t> result;
    result.reserve(indices.size());
    const auto appendPass = [&](bool forceSightObjects) {
        for (const auto index : indices) {
            const bool belongsToForceSightPass =
                (entries[index].modelClassification &
                 (kK2ModelDoor | kK2ModelCharacter)) != 0u;
            if (forceSight && belongsToForceSightPass != forceSightObjects) continue;
            result.push_back(entries[index].sourceIndex);
        }
    };
    appendPass(false);
    if (forceSight) appendPass(true);
    return result;
}

std::vector<std::size_t> k2StaticMeshBucketOrder(
    const std::vector<K2StaticMeshBucketEntry>& entries) {
    return staticMeshBucketOrder(entries);
}

std::vector<std::size_t> k2GobBucketDrawOrder(
    const std::vector<K2GobBucketEntry>& entries,
    bool newSorting, bool forceSight) {
    return gobBucketDrawOrder(GameVersion::Kotor2, entries, newSorting, forceSight);
}

bool ownerTreeHasTransparency(const neomdl::model::Model& model,
                              const neomdl::model::Pose* pose) noexcept {





    const auto inspect = [&](const auto& self, std::size_t nodeIndex) -> bool {
        if (nodeIndex >= model.nodes.size()) return false;
        const auto& node = model.nodes[nodeIndex];
        if (node.mesh && *node.mesh < model.meshes.size()) {
            const float alpha = pose && nodeIndex < pose->nodeMaterials.size()
                ? pose->nodeMaterials[nodeIndex].alpha
                : neomdl::model::bindNodeMaterial(node).alpha;
            // C++ ordered comparison matches ucomiss/ja: NaN is not transparent.
            if (1.0f > alpha) return true;
        }
        for (const auto child : node.children)
            if (self(self, child)) return true;
        return false;
    };

    bool foundRoot = false;
    for (std::size_t index = 0u; index < model.nodes.size(); ++index) {
        if (model.nodes[index].parent) continue;
        foundRoot = true;
        if (inspect(inspect, index)) return true;
    }
    // Malformed models can lack a valid root. Inspect each node independently
    // rather than silently treating such an owner as opaque.
    if (!foundRoot)
        for (std::size_t index = 0u; index < model.nodes.size(); ++index)
            if (inspect(inspect, index)) return true;
    return false;
}

} // namespace neomdl::model
