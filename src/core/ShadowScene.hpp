#pragma once
#include "core/SceneOwners.hpp"
#include "model/Shadow.hpp"
#include "model/Trigger.hpp"
#include <map>

namespace neomdl {
class ModelDocument;
class ReferenceScene;
class AreaScene;
enum class SceneShadowInputMode { SceneFocus, Explicit };
struct SceneShadowInputs {
    SceneShadowInputMode mode{SceneShadowInputMode::SceneFocus};
    // Explicit inspection inputs. Engine Scene owns c_FocusGob, whose light
    // container is independent of every player/caster (K2 Scene +0x2bc).
    // Null in Explicit mode selects the separate inspection container.
    std::optional<LightOwnerId> containerOwner;
    LightQuerySphere containerSphere{{0,0,0},1.0f};
    LightQuerySource containerSource{LightQuerySource::Room};
    model::Vec3 scenePosition{}; // fade center and 1.75 light-height guard
    model::Vec3 casterRegion{};  // K2-only 1100 squared-distance gate
    std::optional<float> maximumCasterDistance; // engine shadowdistance input
};
using SceneShadowCaster = SceneOwnerView;
std::vector<SceneShadowCaster> collectSceneShadowCasters(
    const ModelDocument&, const model::Pose&, const ReferenceScene&, const AreaScene&,
    bool includeReferences = true, float objectLightRadius = 1.0f,
    const model::DanglySimulationState* mainDangly = nullptr,
    const ParticleScene* inspectionParticles = nullptr);
enum class ShadowCasterDecision { Admitted, Disabled, Hidden, Classification, Region, Distance, LightRange };
struct ShadowCasterResult {LightOwnerId owner{};ShadowCasterDecision decision{};};
// The engine shadow bucket remains live while behavior callbacks animate or
// destroy owners. load must return the current slot's post-animation view.
struct SceneShadowOwnerAdmission {
    std::function<std::size_t()> count;
    std::function<SceneShadowCaster(std::size_t)> load;
    // The host checks Gob::GetBehavior before invoking its Animate path.
    std::function<void(LightOwnerId)> animate;
};
struct SceneShadowFrame {
    std::optional<ActiveLight> light;
    std::vector<model::ShadowVolume> volumes;
    model::Mesh fadeFan;
    std::vector<ShadowCasterResult> casters;
    std::string diagnostic;
};
void validateSceneShadowInputs(const SceneShadowInputs& inputs);
// The registry contains borrowed frame views. replaceCasters is called after
// pose/query synchronization, in registration order, before scene execution.
class ShadowScene final {
public:
    static constexpr LightOwnerId independentContainer=0xfffffff000000002ull;
    static constexpr LightOwnerId focusContainer=0xfffffff000000003ull;
    void setInputs(SceneShadowInputs inputs);
    const SceneShadowInputs& inputs() const noexcept {return inputs_;}
    void replaceCasters(std::vector<SceneShadowCaster> casters);
    const std::vector<SceneShadowCaster>& casters() const noexcept {return casters_;}
    void setCasterEnabled(LightOwnerId owner,bool enabled);
    bool casterEnabled(LightOwnerId owner) const noexcept;
    // Scene::Render moves its embedded c_FocusGob before ManageSceneBSP.
    // It keeps the Gob constructor radius/bounds: no Part tree is installed.
    void setSceneFocus(model::Vec3 position) noexcept {sceneFocus_=position;}
    model::Vec3 sceneFocus() const noexcept {return sceneFocus_;}
    static model::Bounds focusBounds() noexcept;
    bool focusVisible(const AreaScene&,
                      const std::vector<model::TriggerPlane>& frustum) const;
    void retainFocusContainer(LightScene& lights) const;
    // Call at c_FocusGob's sorted DoGobBuckets entry, even when shadows are
    // disabled. RenderShadows precedes that entry and reads retained weights.
    void updateFocusContainer(LightScene& lights) const;
    void retainContainer(LightScene& lights) const;
    void clear(LightScene& lights);
    // CPU-only preparation is transactional: the renderer receives a complete
    // frame or no frame, never a partly submitted stencil pass.
    SceneShadowFrame buildFrame(LightScene& lights,const model::Mat4& cameraView,
                                float farClip,const SceneShadowInputs* frameInputs=nullptr,
                                const SceneShadowOwnerAdmission* admission=nullptr) const;
private:
    SceneShadowInputs inputs_;
    model::Vec3 sceneFocus_{};
    std::vector<SceneShadowCaster> casters_;
    std::map<LightOwnerId,bool> enabled_;
    // Numeric instance IDs can be reused after an area/document replacement.
    // An expired selection must be explicitly chosen again, never rebound.
    bool selectedOwnerLost_{};
};
} // namespace neomdl
