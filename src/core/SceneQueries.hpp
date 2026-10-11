#pragma once

#include "model/Model.hpp"
#include "model/SceneOrdering.hpp"
#include "model/Trigger.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace neomdl {
class ModelDocument;
class ReferenceScene;
class AreaScene;
class TriggerScene;
class ParticleScene;






using HitOwnerId = std::uint64_t;
using HitRoomId = std::uint64_t;
using HitPartTransform = model::EngineFrame;
struct HitSphere {
    model::Vec3 center{}; // Gob-local, NOT automatically a world/render bound
    float radius{1.0f};
};
struct SceneHitInfo {
    // CHitInfo's inputs survive ClearOutput. Explicitly supplied info is NOT
    // cleared by Scene::HitCheck; use clearOutput()/reset() when starting anew.
    // hit() returns this retained type, and always replaces owner with the
    // current query's Gob result (null for no dynamic hit).
    bool staticOnly{};
    std::uint32_t surfaceMask{0xffffffffu};
    std::vector<HitOwnerId> ignoredOwners;
    bool boundingBoxesOnly{};
    bool ignoreBoxStartInside{};
    bool classificationFilter{};
    bool checkAlpha{true};
    bool excludeRuntimeFlag{};
    bool skipStatic{};
    bool includeClassificationOne{};

    enum class Type : std::uint8_t { None, Static, Owner };
    Type type{Type::None};
    model::Vec3 position{};
    model::Vec3 localPosition{};
    std::uint32_t surface{};
    std::optional<HitOwnerId> owner;
    std::int32_t face{-1};
    model::Vec3 normal{}; // ordinary TriMesh hits intentionally return zero
    std::string part;
    std::uintptr_t triggerOwner{};
    std::optional<HitRoomId> room;

    void clearOutput() noexcept;
    void reset();
};
struct SceneHitSettings {
    // Engine process-global defaults, not inferred from draw visibility.
    bool staticTriMeshes{};
    bool staticAabb{true};
    bool aabbTestOpposite{};
    bool floorFallback{};
    float minimumOwnerAlpha{0.25f};
    model::Vec3 boxExpansion{};
};
// Snapshot of current LOCAL dynamic positions, indexed like Model::meshes.
// An absent pool is an unwritten/missing engine allocation, not MDX geometry.
using AnimMeshHitPools = std::vector<std::optional<std::vector<model::Vec3>>>;
AnimMeshHitPools engineAnimMeshHitPools(const model::Model&, const model::Pose&);

struct HitOwner {
    HitOwnerId id{};
    const model::Model* model{}; // caller must remove before destroying asset
    std::vector<HitPartTransform> parts;
    AnimMeshHitPools animmeshPositions;
    model::Vec3 position{};
    model::Quaternion orientation{};
    float scale{1.0f};
    HitSphere sphere{}; // explicit engine broad phase; never derived at query time
    float alpha{1.0f}; // retained SetColorShifting alpha, independent of enable
    float doorAlpha{1.0f}; // separate field selected by classification bit 0x08
    bool ignoreHitCheck{}; // scene-only distortion exclusion (legacy host name)
    bool runtimeExclusionFlag{}; // actual Gob::SetIgnoreHitCheck input
};
struct HitStaticPart {
    const model::Model* model{};
    std::size_t node{};
    // SpawnRoom's collapsed part frame. No children are traversed a second time.
    HitPartTransform transform{};
    model::Bounds worldBounds{}; // ComputePartBBox: rounded MDX drawing positions
    std::optional<std::vector<model::Vec3>> animmeshPositions;
};
struct HitRoom {
    HitRoomId id{};
    model::Bounds bounds{};
    std::vector<model::TriggerVolume> triggers;
    std::vector<HitStaticPart> meshes;
    std::optional<HitStaticPart> aabb; // engine room node has ONE, last assignment wins
};
struct HitBspNode {
    // Optional explicit BSP input. InitBSP is empty in the engine;
    // NeoMDL does not synthesize a replacement partition tree.
    std::optional<HitRoom> contents;
    bool leaf{true};
    model::Vec3 normal{};
    float distance{};
    std::optional<std::size_t> negative;
    std::optional<std::size_t> positive;
};

// Live CSWCObject inputs for CSWCCreature::CanSee. These are game-object
// identities/heights, not model classification or geometry-derived bounds.
struct GameSightActor {
    model::Vec3 position{};
    float height{}; // target object's virtual height result; observer uses +1.5
    std::optional<HitOwnerId> bodyOwner,headOwner;
};
struct GameSightObject {
    enum class Kind {Other,Door,Placeable};
    Kind kind{Kind::Other};
    std::optional<HitOwnerId> bodyOwner;
    // nullopt is an absent engine server door, not an unknown loaded value.
    std::optional<std::uint8_t> currentServerDoorOpenState;
};
// nullopt is CAurObject::GetUserData()==null. A supplied object must retain
// the actual body-owner identity used by GetAurObject(255,true).
using GameSightLookup=std::function<std::optional<GameSightObject>(HitOwnerId)>;

// Helpers expose the exact directed primitive contracts for deterministic hosts.
// Box: 0 miss, -1 start inside (output untouched), 1..6 entry face (X/Y/Z order).
int engineHitBox(model::Vec3 start, model::Vec3 end, model::Bounds bounds,
                 model::Vec3* output = nullptr) noexcept;
bool engineHitSphere(model::Vec3 start, model::Vec3 end, HitSphere sphere) noexcept;
// Plane derived from the current triangle, one-sided, zero edge tolerance.
// Shared by animmesh geometry and engine WOK room selection.
bool engineHitTriangle(const std::array<model::Vec3,3>& vertices,
                       model::Vec3 start,model::Vec3& end);
model::Vec3 engineNormalize(model::Vec3) noexcept;
model::Quaternion engineRotationArc(model::Vec3 from,model::Vec3 to) noexcept;
model::Vec3 engineRotate(model::Quaternion orientation, model::Vec3 vector) noexcept;
model::Quaternion engineInverse(model::Quaternion orientation) noexcept;
// Rejects sheared/scaled host matrices: Gob scale is a separate engine input.
HitPartTransform engineRigidFrame(const model::Mat4& frame);
// Part::GetWorld, shared by camera, reference and track/modelhook callers.
HitPartTransform enginePartWorld(const model::Model&,const model::Pose&,
    const model::Mat4& ownerFrame,float ownerScale,std::size_t node);
std::vector<HitPartTransform> engineHitPose(const model::Model& model,
                                           const model::Pose* pose);
// CollapsePartTree (Desktop 0xc6f5a): compose raw rotation/translation without
// parent scales, preserving EACH part's scale for the later hit consumer.
std::vector<HitPartTransform> engineCollapsedHitPose(const model::Model& model,
    const model::Pose& pose,const model::EngineFrame& ownerFrame);
// Explicit rigid-matrix inspection adapter.
std::vector<HitPartTransform> engineCollapsedHitPose(const model::Model& model,
    const model::Pose& pose,const model::Mat4& ownerFrame);
// Explicit counterpart of Gob::GetMinimumSphere (Desktop 0xae526), including its
// 1.35 multiplier and engine child-sphere merge, NOT a render-geometry proxy.
HitSphere engineMinimumHitSphere(const model::Model& model,
    const std::vector<HitPartTransform>& parts, float ownerScale = 1.0f);
// NewCAurObject -> Animate(0) -> ProcessGob computes the cached sphere before
// animation controllers run. Classification-one emitters still have their
// constructor radius 1 at this point; later poses do not rebuild this cache.
// K1 Desktop shares the construction arithmetic, but its explicit later dirty
// processing recomputes without K2's radius-equals-one gate.
HitSphere engineConstructedOwnerSphere(const model::Model& model);

// ProcessParts' Gob ordering input: distance to the rotated/translated cached
// owner-sphere center. Its construction scale is already present in the sphere.
float engineOwnerCameraDistance(HitSphere sphere,model::Vec3 ownerPosition,
    model::Quaternion ownerOrientation,model::Vec3 cameraPosition) noexcept;

// ComputePartBBox (Desktop 0xc6aa7): unscaled MDX positions, rigid frame,
// Round(x,.001) = .001*floor(x/.001+.5). This is not the CPU hit pool.
model::Bounds engineStaticHitBounds(const model::Mesh&, const HitPartTransform&);
std::vector<HitStaticPart> engineStaticHitParts(const model::Model&, const model::Pose&,
    const model::EngineFrame&, const neoshared::bif::JobControl& job = {});

class SceneQueries final {
public:
    void clear() noexcept;



    // Main/reference/static-room host adapter. Registration uses all live
    // owners and rooms, independently of renderer visibility. A new ordinary
    // owner receives its constructed GetMinimumSphere state; later updates
    // preserve that sphere and externally set Gob query flags/alpha. Allocated
    // detached Gobs retain those fields until their assets are destroyed.
    void synchronize(const ModelDocument&, const model::Pose&, const ReferenceScene&,
                     const AreaScene&, const TriggerScene&,
                     const ParticleScene* inspectionParticles = nullptr);
    void registerOwner(HitOwner owner); // stable insertion order on replacement
    bool removeOwner(HitOwnerId id) noexcept;
    // Direct Gob lookup/hit remains available while SetScene(null) has removed
    // the object from the scene's owner list.
    const HitOwner* owner(HitOwnerId id) const noexcept;
    HitOwner* owner(HitOwnerId id) noexcept;
    void setRooms(std::vector<HitRoom> rooms); // all registered rooms, NOT VIS subset
    void setBsp(std::vector<HitBspNode> nodes, std::optional<std::size_t> root);
    const std::vector<HitOwner>& owners() const noexcept { return owners_; } // registered only
    const std::vector<HitRoom>& rooms() const noexcept { return rooms_; }
    SceneHitSettings& settings() noexcept { return settings_; }
    const SceneHitSettings& settings() const noexcept { return settings_; }
    std::uint64_t revision() const noexcept { return revision_; }

    // Endpoint shortening preserves engine nearest/tie semantics. Invalid
    // admitted geometry throws ModelError; it must not become "unoccluded".
    bool hit(model::Vec3 start, model::Vec3 end, SceneHitInfo& info) const;
    std::optional<SceneHitInfo> hit(model::Vec3 start, model::Vec3 end) const;
    // Game SceneHitCheck (K1 Desktop 0x6ff40 / K2 Desktop 0x2886f6): server
    // WOKs shorten the segment before the visual query, whose static AABB
    // stage is disabled for this call. A WOK-only hit returns Static without
    // writing WOK data into info; the engine Vector* parameter is unused.
    // The returned 0/1/2 type is independent of info.type for WOK-only hits.
    SceneHitInfo::Type gameHit(const AreaScene& area,model::Vec3 start,
                               model::Vec3 end,SceneHitInfo& info) const;
    // K1 0x80d10 / K2 0x28885c. The caller owns the persistent engine hit-info
    // inputs; this caller clears outputs/ignore list and sets its class filter.
    // A null target is the engine null-CSWCObject case.
    bool creatureCanSee(const AreaScene& area,const GameSightActor& observer,
        const GameSightActor* target,const GameSightLookup& lookup,
        SceneHitInfo& info,model::GameVersion game) const;
    // Explicit root selects the engine Desktop HitCheckPart caller, which starts
    // the selected Part at scale one instead of the ordinary Gob scale.
    bool hitOwner(HitOwnerId id, model::Vec3 start, model::Vec3 end,
                  SceneHitInfo& info, std::optional<std::size_t> root = {}) const;
private:
    void updateOwnerRegistration(HitOwner owner, bool registered);
    bool hitContents(const HitRoom& room, model::Vec3 start, model::Vec3& end,
                     SceneHitInfo& info, bool leaf,bool staticAabb) const;
    bool hitVisual(model::Vec3 start,model::Vec3 end,SceneHitInfo& info,
                    bool staticAabb) const;
    std::vector<HitOwner> owners_;
    std::vector<HitOwner> detachedOwners_;
    std::vector<HitRoom> rooms_;
    std::vector<HitBspNode> bsp_;
    std::optional<std::size_t> root_;
    SceneHitSettings settings_;
    std::uint64_t revision_{};
    std::vector<HitOwnerId> hostOwners_;

};

} // namespace neomdl
