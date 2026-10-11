#pragma once
#include "core/LightScene.hpp"
#include "core/SceneOwnerLifetime.hpp"
#include "model/Model.hpp"
#include <functional>
namespace neomdl {
class ModelDocument;
class ReferenceScene;
class AreaScene;
class ParticleScene;
class SceneAnimationOwner;
// Borrowed per-frame owner registration shared by scene consumers. Admission
// to shadows, motion blur or drawing is separate from registration.
struct SceneOwnerView {
    LightOwnerId id{};
    std::string label;
    const model::Model* asset{};
    const ModelDocument* sourceDocument{};
    const model::Pose* pose{};
    const model::DanglySimulationState* dangly{};
    const SceneAnimationOwner* animationOwner{};
    model::EngineFrame frame{};
    model::Mat4 world{model::identityMatrix()};
    float scale{1.0f};
    LightQuerySphere lightSphere;
    bool visible{true}, enabled{true}; // shadow admission view, not a render callback
    bool renderVisible{true}; // normal area membership; no shadow classification shortcut
    // In-cutscene cutscenedummy position affects range checks, never the
    // original Gob-origin/region gate or the projected owner transform.
    std::optional<model::Vec3> rangePosition;
    const std::vector<std::uint8_t>* enabledParts{};
    SceneOwnerLifetime ownerLifetime;
    // Late engine consumers read the retained Gob's current transform. A
    // recycled/pool-owned chunk remains the same owner after SetScene(null).
    std::function<model::EngineFrame()> resolveFrame;
    std::function<model::Mat4()> resolveWorld;
    std::function<float()> resolveScale;
    // DoGobBuckets/DoGobShadows read the current cutscenedummy only after
    // owner animation; absence retains the Gob origin at these callers.
    std::function<std::optional<model::Vec3>()> resolveRangePosition;
    // The same Gob.previous-frame field supplies blur and owner point wind.
    std::function<model::EngineFrame()> resolvePreviousRenderedFrame;
    std::function<void(const model::EngineFrame&)> commitRenderedFrame;
};
// Same borrowed registration path for the wx host and headless scenes. Static
// room Parts are not Gobs; a module-only document does not invent a main owner.
std::vector<SceneOwnerView> collectSceneOwners(
    const ModelDocument& document, const model::Pose& pose,
    const ReferenceScene& references, const AreaScene& area,
    bool includeReferences = true, float objectLightRadius = 1.0f,
    const model::DanglySimulationState* mainDangly = nullptr,
    const ParticleScene* inspectionParticles = nullptr,
    SceneAnimationOwner* mainAnimationOwner = nullptr);
// ChunkyParticle owns a separate Gob. SetScene(null) removes that Gob, while
// still-listed descendants retain their own scene membership and lifetime.
std::vector<SceneOwnerView> collectParticleChunkOwners(
    const ParticleScene* inspectionParticles, const AreaScene& area,
    bool includeReferences = true, bool registeredOnly = true);
} // namespace neomdl
