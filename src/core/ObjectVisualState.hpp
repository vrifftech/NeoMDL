#pragma once

#include "core/MotionBlurScene.hpp"
#include "core/OwnerEffects.hpp"
#include "core/ResourceContext.hpp"
#include "model/Model.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neomdl {
class ModelDocument;

enum class ObjectVisualStateSource : std::uint8_t {
    AuthoredGit,
    LiveAdapter,
    AnimationEvent,
    CapturedTrace,
};
const char* objectVisualStateSourceName(ObjectVisualStateSource) noexcept;

enum class ObjectAnimationFamily : std::uint8_t { Door, Placeable, Trigger };

// Numeric server/client animation identities are authoritative. The textual
// name is only a scoped animations.2da projection and is never guessed from a
// serialized OpenState/AnimationState value.
struct ObjectAnimationCommand {
    ObjectAnimationFamily family{ObjectAnimationFamily::Placeable};
    std::uint8_t serializedState{};
    std::uint16_t serverAnimation{};
    std::vector<std::uint16_t> clientCandidates;
    std::vector<std::string> candidateNames;
    std::optional<std::uint16_t> clientAnimation;
    std::string animationName;
    bool enginePlaceablePolicy{}, playEmpty{};
    float playbackRate{1.0f};
    std::uint32_t runFlags{};
    float startTime{};
    // CSWCDoor +0x11c is copied into CSWCAnimBaseDoor +0xdc when its
    // animation owner is constructed. The engine door constructor sets 1.
    std::int32_t doorAnimationSelector{1};
    std::string diagnostic;

    bool resolved() const noexcept {
        return clientAnimation.has_value() && (playEmpty || !animationName.empty());
    }
};
ObjectAnimationCommand doorOpenStateAnimation(std::uint8_t state,
                                               std::int32_t engineSelector = 1);
ObjectAnimationCommand placeableAnimationState(std::uint8_t state);
ObjectAnimationCommand placeableServerAnimation(std::uint16_t serverAnimation);
ObjectAnimationCommand triggerServerAnimation(std::uint16_t serverAnimation);
// Reads the selected module/installations's animations.2da and then requires a
// unique candidate that is actually present in the supplied model hierarchy.
// Failure leaves the numeric command intact and returns false with diagnostic.
bool resolveObjectAnimation(ObjectAnimationCommand&, const ResourceResolver&,
                            const ModelDocument&, std::string& diagnostic);

struct ObjectColorShift {
    bool enabled{};
    model::Vec3 color{1.0f,1.0f,1.0f};
    float amount{1.0f};
    bool propagateToReferences{true};
};

// A target route is source-emitter selective and target-owner explicit. An
// empty emitter name reproduces a caller that addresses every target-dependent
// emitter in the selected source owner. targetOwner is resolved every frame;
// explicitWorld is a retained inspection/capture input. Exactly one is set.
struct ObjectEmitterTargetRoute {
    std::string emitterName;
    bool allSourceOwners{true};
    std::optional<std::size_t> sourceReference;
    std::optional<std::uint64_t> targetOwner;
    std::string targetNode;
    std::optional<model::Mat4> explicitWorld;
};

struct ObjectVisualStateLayer {
    std::optional<ObjectAnimationCommand> animation;
    std::optional<ObjectColorShift> colorShift;
    std::optional<float> opacity;
    std::optional<std::string> environmentMap;
    std::optional<float> textureRotationDegrees;
    std::optional<bool> motionBlur;
    std::optional<float> maximumBlurLength;
    std::optional<bool> distortion;
    std::optional<bool> hologram;
    // Explicit inspection override. The engine producer is the owning model's
    // serialized classification bit 0; ParticleScene derives it per owner.
    std::optional<bool> forceEmitterRadiusUnbounded;
    // An engaged empty vector explicitly clears inherited/authored routes.
    std::optional<std::vector<ObjectEmitterTargetRoute>> emitterTargets;
};

struct EffectiveObjectVisualState {
    std::optional<ObjectAnimationCommand> animation;
    ObjectColorShift colorShift;
    float opacity{1.0f};
    std::string environmentMap;
    float textureRotationDegrees{};
    MotionBlurOwnerSettings motionBlur;
    bool distortion{};
    bool hologram{};
    bool forceEmitterRadiusUnbounded{};
    std::vector<ObjectEmitterTargetRoute> emitterTargets;

    OwnerVisualEffects ownerEffects() const;
};

struct ObjectVisualTraceEntry {
    std::uint64_t sequence{};
    ObjectVisualStateSource source{ObjectVisualStateSource::AuthoredGit};
    std::string operation;
    std::string detail;
    bool applied{};
};

bool validObjectVisualStateLayer(const ObjectVisualStateLayer&, std::string& error) noexcept;

// Two-layer state used by the viewer/replay adapter. Live values override only
// the fields they explicitly carry; clearing the live layer reveals authored
// state. Changes are transactional and retained in a deterministic trace.
class ObjectVisualStateAdapter final {
public:
    bool setAuthored(const ObjectVisualStateLayer&, std::string& error);
    bool setLive(const ObjectVisualStateLayer&, std::string& error);
    void clearLive();

    const ObjectVisualStateLayer& authored() const noexcept { return authored_; }
    const ObjectVisualStateLayer& live() const noexcept { return live_; }
    EffectiveObjectVisualState effective() const;
    const std::vector<ObjectVisualTraceEntry>& trace() const noexcept { return trace_; }
    void note(ObjectVisualStateSource source, std::string operation,
              std::string detail, bool applied);

private:
    ObjectVisualStateLayer authored_, live_;
    std::uint64_t nextSequence_{1u};
    std::vector<ObjectVisualTraceEntry> trace_;
};

enum class ObjectVisualEventKind : std::uint8_t {
    Animation,
    Detonate,
    RotateTexture,
    SetEmitterTarget,
    ClearEmitterTargets,
    SetMotionBlur,
    SetDistortion,
    SetHologram,
    SetOpacity,
    SetEnvironmentMap,
    SetColorShift,
    SetForceEmitterRadiusUnbounded,
    ClearLiveOverrides,
};

// Explicit viewer/replay event. Unused fields are ignored according to kind;
// this is a state adapter, not a gameplay or scripting event packet.
struct ObjectVisualEvent {
    ObjectVisualEventKind kind{ObjectVisualEventKind::Detonate};
    std::optional<ObjectAnimationCommand> animation;
    std::optional<ObjectEmitterTargetRoute> target;
    ObjectColorShift colorShift;
    std::string text;
    float value{};
    float secondaryValue{1.5f};
    bool enabled{};
};
const char* objectVisualEventName(ObjectVisualEventKind) noexcept;

} // namespace neomdl
