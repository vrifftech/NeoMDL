#pragma once

#include "core/ModelDocument.hpp"
#include "core/CreatureLipSync.hpp"
#include "core/LookAtAnimation.hpp"
#include "core/SceneAnimation.hpp"
#include "model/Emitter.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace neomdl {

// A null reference index identifies the owning/main Gob. A populated index
// identifies one of the separately owned referenced CAurObjects.
struct ReferenceObjectTarget {
    std::optional<std::size_t> reference;
    std::string nodeName;
};

struct RuntimeReferenceInstance {
    SceneAnimationOwner animationOwner;
    std::size_t definitionIndex{};
    neomdl::model::Pose pose;
    neomdl::model::DanglySimulationState danglyState;
    neomdl::model::EngineFrame frame{};
    neomdl::model::Mat4 world = neomdl::model::identityMatrix();
    std::optional<neomdl::model::AnimationReference> defaultAnimation;
    std::optional<neomdl::model::AnimationRuntime> animationRuntime;
    LookAtAnimation lookAt;
    float playbackRate{1.0f};
    float animationTime{};
    float initialAnimationTime{};

    std::optional<std::size_t> targetReference;
    std::size_t targetNodeIndex{};
    std::string targetNodeName;
    bool attached{};
    bool reattached{};
    bool active{};
    bool sceneMember{true};
    float scale{1.0f};
    std::uint64_t attachmentOrder{};
    // AddAttachment retains list position when a behavior is replaced on the
    // same target, unlike its newly registered UpdateScenes listener.
    std::uint64_t scaleAttachmentOrder{};
    std::uint64_t sceneRegistration{};
    bool isAnimating() const noexcept {
        return animationOwner.isAnimating();
    }
};

struct ReferenceAnimationEvent {
    std::size_t reference{};
    neomdl::model::AnimationRuntimeEvent event;
};

struct ReferenceReattachResult {
    std::size_t eligible{};
    std::size_t attached{};
    std::size_t detached{};
};

// Runtime counterpart of K1 Desktop/K2 PartReference/CreateReferenceObjects. Referenced
// models are separately owned objects: they have independent default-animation
// clocks and receive a rigid position/orientation attachment to a target part.
class ReferenceScene final {
public:
    void setDocument(const ModelDocument* document);
    void setSceneAnimation(std::shared_ptr<SceneAnimation>);
    void setOwnerSceneMember(bool);
    void setParentAnimationOwner(SceneAnimationOwner* owner,const model::AnimationRuntime* runtime) noexcept {
        parentAnimationOwner_=owner;parentAnimationRuntime_=runtime;
    }
    void setAnimationEventCallback(std::function<void(const ReferenceAnimationEvent&)> callback) {animationEvent_=std::move(callback);}
    void setOwnerAnimated(std::function<void(std::size_t,float)> callback) {ownerAnimated_=std::move(callback);}
    void setPartsAnimated(std::function<void(std::size_t,float)> callback) {partsAnimated_=std::move(callback);}
    SceneAnimationOwner& animationOwner(std::size_t index) {return instances_[index].animationOwner;}
    const SceneAnimationOwner& animationOwner(std::size_t index) const {return instances_[index].animationOwner;}
    void simulateDanglyInstance(std::size_t,float,const model::DanglyEnvironment&);

    // Reference construction shares the scene random stream. Without a supplied
    // stream, the seed selects NeoMDL's existing deterministic replay policy.
    void reset(std::uint32_t randomSeed = 1u);
    void setSceneRandom(model::ParticleRandom* random) noexcept { sceneRandom_ = random; }
    // Runtime owner/controller changes refresh the inspection cache without
    // moving independently animated child Gobs. Explicit inspection requests
    // may reconstruct attachment frames immediately.
    void synchronize(const neomdl::model::Pose& mainPose,bool resolveAttachments=true);
    // Position/orientation of the containing scene Gob. Reference attachments
    // and specialized part worlds include this frame exactly once.
    void setOwnerFrame(const model::EngineFrame& frame,bool resolveAttachments=true);
    // Explicit matrix inspection adapter; engine producers retain raw frames.
    void setOwnerWorld(const neomdl::model::Mat4& world,bool resolveAttachments=true);
    const model::EngineFrame& ownerFrame() const noexcept { return ownerFrame_; }
    const model::EngineFrame& mainFrame() const noexcept { return mainFrame_; }
    model::EngineFrame enginePartFrame(std::optional<std::size_t> reference,
                                      std::size_t node) const;
    void setOwnerScale(float scale);
    float ownerScale() const noexcept { return ownerScale_; }
    const neomdl::model::Mat4& ownerWorld() const noexcept { return ownerWorld_; }
    // Called after each independent Gob's animation, before the containing
    // owners publish their new attachment frames at the end of this traversal.
    void advance(const neomdl::model::Pose& mainPose, float elapsedSeconds,
                 neomdl::model::ParticleRandom& random,
                 const std::function<void(std::size_t)>& ownerAnimated = {});
    std::vector<ReferenceAnimationEvent> takeAnimationEvents();
    // Wield forwards ordinary named commands to the held models before it
    // attempts the body command. The separate overlay entrypoint omits items.
    void playCreatureEquippedAnimation(const std::string& name, float playbackRate,
                                       std::uint32_t flags, float startTime);
    // Creature heads are separately created Gobs attached at HEADHOOK, not
    // PartReferences with randomized default-animation clocks.
    void playCreatureHeadAnimation(const std::string& name, float playbackRate,
                                   std::uint32_t flags, float startTime);
    void setCreatureHeadVisemes(const CreatureVisemes&);
    void stopCreatureHeadVisemes();
    void beginCreatureHeadLookAt(const std::string& sourceBone,LookAtTargetResolver,
        float horizontal,float vertical,float transition,float maximumDistance);
    void endCreatureHeadLookAt();
    void resetDangly();
    // Runs PartDanglyMesh after attachment worlds have been resolved. The
    // Each reference supplies its retained absolute raw owner frame; the
    // environment supplies wind/profile independently of containing owners.
    void simulateDangly(float elapsedSeconds,
                        const neomdl::model::DanglyEnvironment& environment);

    ReferenceReattachResult reattachOwnedReferences(
        std::optional<std::size_t> ownerReference,
        std::optional<ReferenceObjectTarget> target);
    ReferenceReattachResult restoreOwnedReferences(
        std::optional<std::size_t> ownerReference) {
        return reattachOwnedReferences(ownerReference, std::nullopt);
    }

    const std::vector<RuntimeReferenceInstance>& instances() const noexcept {
        return instances_;
    }
    const RuntimeReferenceInstance* instance(std::size_t index) const noexcept;
    const std::vector<std::size_t>& sceneRegistrationOrder() const noexcept {
        return sceneRegistrationOrder_;
    }
    const neomdl::model::Mat4& mainWorld() const noexcept { return mainWorld_; }
    neomdl::model::Bounds bounds() const noexcept;
    const std::vector<std::string>& diagnostics() const noexcept { return diagnostics_; }
    std::uint64_t revision() const noexcept { return revision_; }

private:
    void resetInstances(std::uint32_t randomSeed, bool constructOwners);
    bool advanceInstance(std::size_t,float,bool,model::ParticleRandom&);
    void initializeInstance(std::size_t index, model::ParticleRandom& random,
                            std::vector<std::uint8_t>& state, bool constructOwners);
    void setAuthoredAttachment(std::size_t index);
    void bindAttachmentControl(std::size_t index);
    void setInstanceSceneMember(std::size_t index, bool member);
    void propagateAttachedScales(std::optional<std::size_t> parent,float scale);
    void captureMainPose(const model::Pose&);
    void resolveWorlds();
    bool resolveWorld(std::size_t index, std::vector<std::uint8_t>& state,
                      bool liveTargets=false);
    std::optional<std::size_t> findNode(
        const ReferenceObjectTarget& target) const noexcept;
    void turnOwnerToward(std::optional<std::size_t> ownerReference,
                         std::size_t childReference);

    const ModelDocument* document_{};
    model::ParticleRandom* sceneRandom_{};
    model::ParticleRandom ownedRandom_;
    std::shared_ptr<SceneAnimation> sceneAnimation_=std::make_shared<SceneAnimation>();
    bool ownerSceneMember_{true};
    SceneAnimationOwner* parentAnimationOwner_{};
    const model::AnimationRuntime* parentAnimationRuntime_{};
    std::function<void(std::size_t,float)> ownerAnimated_,partsAnimated_;

    bool engineRuntime_{};
    std::vector<ReferenceAnimationEvent> animationEvents_;
    std::function<void(const ReferenceAnimationEvent&)> animationEvent_;
    std::vector<std::size_t> sceneRegistrationOrder_;
    std::uint64_t nextAttachmentOrder_{1u};
    std::uint64_t nextSceneRegistration_{1u};
    std::vector<RuntimeReferenceInstance> instances_;
    std::vector<std::vector<std::size_t>> children_;
    model::Pose mainPose_;
    model::EngineFrame ownerFrame_{},mainFrame_{};
    neomdl::model::Mat4 ownerWorld_ = neomdl::model::identityMatrix();
    float ownerScale_{1.0f};
    neomdl::model::Mat4 mainWorld_ = neomdl::model::identityMatrix();
    std::vector<std::string> diagnostics_;
    std::uint64_t revision_{};
};

} // namespace neomdl
