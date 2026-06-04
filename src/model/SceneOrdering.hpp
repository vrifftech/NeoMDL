#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace neomdl::model {
struct Model;
struct Pose;
enum class GameVersion;

// K2 Model::classification bits consumed by GobOrderCmp. These names are
// intentionally scoped to the K2 scene runtime rather than inferred
// as a cross-game format contract.
constexpr std::uint16_t kK2ModelEffect = 0x0001u;
constexpr std::uint16_t kK2ModelTile = 0x0002u;
constexpr std::uint16_t kK2ModelCharacter = 0x0004u;
constexpr std::uint16_t kK2ModelDoor = 0x0008u;

// Draw stages observed in the desktop Scene::RenderSinglePass implementations.
// Unsupported viewer stages remain explicit to preserve engine bucket ordering.
enum class K2SceneStage : std::uint8_t {
    FogSetup,
    StaticMeshGeometry,
    StaticTriangleBins,
    TriggerGeometry,
    TriggerHighlightBeforeDynamic,
    Shadows,
    DynamicGobs,
    HologramGobs,
    PersonalSpace,
    Paths,
    TriggerHighlightAfterDynamic,
    Emitters,
    Lightsabers,
    DrawableSquares,
    MotionBlur,
    Distortion,
    FramebufferEffects,
    LensFlares,
    FramebufferEffects2,
};

struct K2SceneStageSequence {
    const K2SceneStage* first{};
    std::size_t count{};

    constexpr const K2SceneStage* begin() const noexcept { return first; }
    constexpr const K2SceneStage* end() const noexcept { return first + count; }
    constexpr std::size_t size() const noexcept { return count; }
    constexpr bool empty() const noexcept { return count == 0u; }
    constexpr K2SceneStage operator[](std::size_t index) const noexcept {
        return first[index];
    }
};

// Stage identifiers are shared; each game selects its desktop caller order.
using EngineSceneStage = K2SceneStage;
using EngineSceneStageSequence = K2SceneStageSequence;
EngineSceneStageSequence engineSceneStageOrder(GameVersion game) noexcept;
K2SceneStageSequence k2SceneStageOrder() noexcept;
const char* k2SceneStageName(K2SceneStage stage) noexcept;

// AddPartToMeshBuckets sends background geometry to one insertion-ordered
// list. Other static triangle meshes are bucketed by diffuse texture TID;
// DoMeshBuckets walks AurTextureGetOrdering(), then draws TID zero last.
struct StaticMeshBucketEntry {
    std::size_t sourceIndex{};
    std::uint64_t textureIdentity{}; // zero is the engine's TID-zero bucket
    std::uint64_t textureOrder{};    // AurTexture ordered-list position
    bool background{};
};

using K2StaticMeshBucketEntry = StaticMeshBucketEntry;

// Identical insertion and texture-order traversal in the K1 and K2
// runtimes. This is shared engine behavior, independent of the Gob comparator.
std::vector<std::size_t> staticMeshBucketOrder(
    const std::vector<StaticMeshBucketEntry>& entries);

std::vector<std::size_t> k2StaticMeshBucketOrder(
    const std::vector<K2StaticMeshBucketEntry>& entries);

// One entry represents one Gob, not one mesh part. Gob::Render retains the
// hierarchy/insertion order of the parts within that owner.
struct GobBucketEntry {
    std::size_t sourceIndex{};
    std::uint64_t ownerIdentity{};
    std::uint64_t insertionOrder{};
    float alpha{1.0f};
    std::uint16_t modelClassification{};
    bool hasTransparency{};
    float cameraDistance{};
};

using K2GobBucketEntry = GobBucketEntry;

// K1 and K2 share the alpha/effect/transparency/distance comparison, but their
// intervening classification priorities differ. K1 tests 0x4, 0x10, then 0x8;
// K2 tests 0x8 then 0x4 and supports the Force Sight ordering/two-pass traversal.
// cameraDistance is the distance to the engine owner minimum-sphere center,
// transformed by owner orientation and position, not a mesh/bounds midpoint.
// Entries are ordered by alpha, classification and distance, then stable owner
// keys. NaN values follow all numeric values. Passing partitionForceSight=false
// leaves the two live owner passes to the renderer.
std::vector<std::size_t> gobBucketDrawOrder(
    GameVersion game,
    const std::vector<GobBucketEntry>& entries,
    bool newSorting = true,
    bool forceSight = false,
    bool partitionForceSight = true);

std::vector<std::size_t> k2GobBucketDrawOrder(
    const std::vector<K2GobBucketEntry>& entries,
    bool newSorting = true,
    bool forceSight = false);

// Per-owner tree query. Both engine queries include hidden mesh Parts; their
// owner pointer gate excludes ownerless/static trees at the caller. Independent
// reference Gobs must be queried separately.
bool ownerTreeHasTransparency(const Model&,const Pose* pose) noexcept;

} // namespace neomdl::model
