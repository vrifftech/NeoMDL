#pragma once

#include "core/LightsaberScene.hpp"
#include "core/ObjectVisualState.hpp"
#include "core/ParticleScene.hpp"
#include "core/ReferenceScene.hpp"
#include "model/AnimationRuntime.hpp"

#include <memory>
#include <optional>
#include <set>
#include <string>

namespace neomdl {

// SpawnRoom retains root -> modelname+"a" as a Gob and collapses the other
// Parts. Indices stay original: SynchronizeTree and skin bone maps are created
// before this operation. A scene view owns no texture payload or GPU resources.
struct RoomModelPartition {
    model::Model staticParts;
    std::optional<model::Model> animatedParts;
    std::optional<std::size_t> animatedRoot;
};
RoomModelPartition partitionK2Room(const model::Model& source);
// The desktop room/owner caller graph supports both games; game-specific
// room visibility and animation behavior remain explicit.
bool engineAreaRuntimeSupported(model::GameVersion game) noexcept;

enum class AreaObjectKind { StaticRoomParts, AnimatedRoom, Track, Creature, Placeable, Door, Weather, Trigger, TrackModel };
const char* areaObjectKindName(AreaObjectKind kind) noexcept;

struct AreaRuntimeServices {
    model::WindManager* wind{};
    model::ParticleRandom* random{};
    const SceneQueries* queries{};

    // Module hosts already know their installation. A raw LYT has no game
    // field, so its first model supplies the game before any owner is built.
    std::optional<model::GameVersion> game{};
    model::AsciiResourceResolver asciiResources{};
    std::shared_ptr<SceneAnimation> animation{};
};
struct AreaFrameSettings {

    bool animate{true};
    bool dangly{true};
    bool emitters{true};
    bool references{true};
};
struct CreatureLoopAnimation {
    std::string name;
    float playbackRate{1.0f};
};

// Stable, independent runtime owner. A static-room container is NOT a Gob:
// its own Parts have a fixed collapsed pose, while referenced Gobs and its
// separately registered emitters retain their engine independent clocks.
class AreaObject final {
public:
    AreaObject(std::size_t firstInstance, AreaObjectKind kind, std::string label,
               std::shared_ptr<ModelDocument> document, const model::Mat4& world,
               std::optional<std::size_t> room, AreaRuntimeServices services = {},
               bool sceneMember = true);
    AreaObject(std::size_t firstInstance, AreaObjectKind kind, std::string label,
               std::shared_ptr<ModelDocument> document, const model::EngineFrame& frame,
               std::optional<std::size_t> room, AreaRuntimeServices services = {},
               bool sceneMember = true);
    AreaObject(const AreaObject&) = delete;
    AreaObject& operator=(const AreaObject&) = delete;
    AreaObject(AreaObject&&) = delete;
    AreaObject& operator=(AreaObject&&) = delete;

    ~AreaObject();
    void setSceneAnimation(std::shared_ptr<SceneAnimation>);
    void setSceneMember(bool);
    SceneAnimationOwner& animationOwner() noexcept {return animationOwner_;}
    const SceneAnimationOwner& animationOwner() const noexcept {return animationOwner_;}
    void prepareAnimationFrame(const AreaFrameSettings& settings) {frameSettings_=settings;}
    void setAnimationEventCallback(std::function<void(const model::AnimationRuntimeEvent&)> callback) {animationEvent_=std::move(callback);}
    bool staticPrimary() const noexcept { return kind_ == AreaObjectKind::StaticRoomParts; }
    AreaObjectKind kind() const noexcept { return kind_; }
    const std::string& label() const noexcept { return label_; }
    std::optional<std::size_t> room() const noexcept { return room_; }
    const std::shared_ptr<ModelDocument>& document() const noexcept { return document_; }
    const model::Pose& pose() const noexcept { return pose_; }
    const model::DanglySimulationState& danglyState() const noexcept { return dangly_; }
    const model::Mat4& world() const noexcept { return references_.mainWorld(); }
    const model::EngineFrame& frame() const noexcept { return references_.mainFrame(); }
    const ReferenceScene& references() const noexcept { return references_; }
    ReferenceScene& references() noexcept { return references_; }
    const model::AnimationRuntime& animation() const noexcept { return animation_; }
    ParticleScene& particles() noexcept { return particles_; }
    const ParticleScene& particles() const noexcept { return particles_; }
    LightsaberScene& lightsabers() noexcept { return lightsabers_; }
    const LightsaberScene& lightsabers() const noexcept { return lightsabers_; }
    std::size_t instanceIndex(std::optional<std::size_t> reference = {}) const;
    std::uint64_t ownerId(std::optional<std::size_t> reference = {}) const;
    double elapsed() const noexcept { return elapsed_; }
    std::uint64_t revision() const noexcept { return revision_; }
    bool paused() const noexcept { return paused_; }
    void setPaused(bool paused) noexcept { paused_ = paused; }
    void setWorld(const model::Mat4& world);
    void setFrame(const model::EngineFrame& frame);
    void appearanceBindingsChanged();
    // CSWCCreature::LoadModel producer; texture-only alignment changes do not call it.
    void initializeCreatureForceSight(std::int16_t alignment,std::uint16_t race);
    void forceAnimateCreatureAppearance();
    void setQueries(const SceneQueries* queries) noexcept { particles_.setSceneQueries(queries); }

    bool applyAuthoredVisualState(const ObjectVisualStateLayer&, std::string& error);
    bool applyLiveVisualState(const ObjectVisualStateLayer&, std::string& error);
    void clearLiveVisualState();
    bool dispatchVisualEvent(const ObjectVisualEvent&, std::string& error);
    const ObjectVisualStateAdapter& visualState() const noexcept { return visualState_; }
    const EffectiveObjectVisualState& effectiveVisualState() const noexcept { return effectiveVisual_; }
    const OwnerVisualEffects* ownerVisualEffects(std::optional<std::size_t> reference = {}) const noexcept;
    const OwnerVisualEffects& engineOwnerVisualInputs(std::optional<std::size_t> reference = {}) const noexcept;
    MotionBlurOwnerSettings motionBlurSettings() const noexcept { return effectiveVisual_.motionBlur; }
    float textureRotationDegrees(const model::Mesh& mesh) const noexcept {
        return mesh.rotateTexture ? effectiveVisual_.textureRotationDegrees : 0.0f;
    }
    // AreaScene resolves owner/node references every frame, then supplies the
    // concrete target transforms without altering the retained authored routes.
    bool setResolvedParticleTargets(std::vector<ParticleEmitterTargetInput>, std::string& error);
    void noteVisualDiagnosticOnce(std::string key, ObjectVisualStateSource source,
                                  std::string operation, std::string detail,
                                  bool applied = false);

    model::AnimationPlayResult play(const std::string& name, float rate = 1.0f,
                                   std::uint32_t flags = 0u, float start = 0.0f);
    // HeadWield overlay caller bypasses equipped-item dispatch, including
    // engine stop flags that do not carry the Overlay bit.
    model::AnimationPlayResult playOverlay(const std::string& name, float rate,
                                          std::uint32_t flags, float start = 0.0f);
    // CSWTrackFollower addresses CAurObject directly, bypassing creature
    // head/equipment animation adapters. Engine start time is zero.
    model::AnimationPlayResult playWorldAnimation(const std::string& name,float rate,std::uint32_t flags);
    const std::optional<CreatureLoopAnimation>& creatureLoopAnimation() const noexcept {
        return creatureLoopAnimation_;
    }
    void setVisemes(const CreatureVisemes&);
    void stopVisemes();
    void beginCreatureLookAt(const std::string& sourceBone,LookAtTargetResolver,
        float horizontal,float vertical,float transition,float maximumDistance);
    void endCreatureLookAt();
    // CSWCRoom::PlayAnimation: scriptloop%02u overlay 0x8; K2 first
    // overlays "null", while K1 calls scriptloop directly.
    model::AnimationPlayResult playRoomAnimation(std::uint16_t number);
    // Gob::AnimateExternal advances its animation/Part tree directly. Weather
    // uses the authored clip length to sample density; no viewer frame clamp,
    // wind tick or PartEmitter particle step belongs to this call.
    void animateExternal(float seconds);
    void reset();
    // All scene owners must complete this phase before query registration and
    // emitter collision sampling. Visibility never suppresses these clocks.
    void advancePose(float seconds, const AreaFrameSettings& settings);
    void advanceEffects(float seconds, const AreaFrameSettings& settings);

private:
    bool advancePrimaryPose(float,bool);
    void advancePrimaryParts(float);
    void rememberCreatureAnimation(const model::AnimationPlayResult&,const std::string&,
                                  float rate,std::uint32_t flags);
    bool commitVisualState(const EffectiveObjectVisualState& previous,
                           ObjectVisualStateSource source, std::string& error);
    void synchronizeParticleVisualInputs(
        std::vector<ParticleEmitterTargetInput> targets = {});
    void refreshAppearanceEffects();
    std::size_t firstInstance_{};
    AreaObjectKind kind_{};
    std::string label_;
    std::optional<std::size_t> room_;
    std::shared_ptr<ModelDocument> document_;
    SceneAnimationOwner animationOwner_;
    std::shared_ptr<SceneAnimation> sceneAnimation_;
    AreaFrameSettings frameSettings_;
    std::function<void(const model::AnimationRuntimeEvent&)> animationEvent_;
    model::AnimationRuntime animation_;
    LookAtAnimation lookAt_;
    std::optional<CreatureLoopAnimation> creatureLoopAnimation_;
    model::Pose pose_;
    model::DanglySimulationState dangly_;
    ReferenceScene references_;
    ParticleScene particles_;
    LightsaberScene lightsabers_;
    ObjectVisualStateAdapter visualState_;
    EffectiveObjectVisualState effectiveVisual_;
    OwnerVisualEffects ownerVisualEffects_;
    material::ForceSightOwnerState creatureForceSight_;
    OwnerVisualEffects referenceOwnerVisualEffects_;
    std::vector<OwnerVisualEffects> referenceAppearanceEffects_;
    std::string appliedAnimationKey_;
    std::set<std::string> visualDiagnosticKeys_;
    bool appearanceCombatPaused_{};
    bool paused_{};
    double elapsed_{};
    std::uint64_t revision_{};
};

} // namespace neomdl
