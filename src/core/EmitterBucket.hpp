#pragma once
#include "core/ParticleScene.hpp"
#include <functional>
namespace neomdl {
struct SceneEmitterDraw {
    ParticleScene* scene{};
    ParticleEmitterInstance* emitter{};
    std::size_t sceneIndex{},emitterIndex{};
};
struct SceneEmitterPrewarmStep {
    std::size_t sceneIndex{},emitterIndex{};
    std::size_t particlesBefore{},particlesAfter{};
    std::uint32_t randomBefore{},randomAfter{};
    bool completed{};
};
struct SceneEmitterPrewarmTrace {
    std::vector<SceneEmitterPrewarmStep> order;
    std::size_t particlesBefore{},particlesAfter{};
    std::uint32_t randomBefore{},randomAfter{};
    bool completed{};
};
struct SceneEmitterFrameSettings {
    double elapsedSeconds{};
    // Scene::IsAnimating is evaluated once before the forward bucket traversal.
    bool sceneAnimating{};
    model::EngineFrame cameraFrame{};
};
struct SceneEmitterFrameStep {
    std::size_t sceneIndex{},emitterIndex{};
    bool sceneGuard{},ownerGuardQueried{},ownerGuard{},animateCalled{},animateCompleted{};
    bool renderCalled{},renderFuncCompleted{},drawCallbackCalled{},drawCallbackCompleted{},renderCompleted{};
    double appliedElapsed{};
    std::size_t particlesBefore{},particlesAfter{};
    std::uint32_t randomBefore{},randomAfterAnimate{},randomAfterRenderFunc{},randomAfterRender{};
};
struct SceneEmitterFrameTrace {
    double requestedElapsed{};
    bool sceneAnimating{},completed{};
    std::vector<SceneEmitterFrameStep> steps;
};
using SceneEmitterRenderCallback=std::function<void(const SceneEmitterDraw&)>;
// Scene::UpdateEmitters is a separate registration-order prewarm before wind
// update and RenderSinglePass. It runs at most once per
// ParticleScene lifetime/reset.
void prewarmSceneEmitterBucket(const std::vector<ParticleScene*>&,
                               SceneEmitterPrewarmTrace* trace=nullptr);
// Evaluate Scene::IsAnimating once, then visit the ordered bucket and call
// AnimateParticles and RenderFunc. Earlier state remains committed if a later
// callback fails.
std::vector<SceneEmitterDraw> executeSceneEmitterBucket(
    const std::vector<ParticleScene*>&,const model::Mat4& view,
    const std::vector<model::EmitterScenePlane>& planes,
    const SceneEmitterFrameSettings&,SceneEmitterFrameTrace* trace=nullptr,
    const SceneEmitterRenderCallback& render={});
// A skipped/disabled scene pass must not bank per-owner animation guards into a
// later frame.
void discardSceneEmitterFrame(const std::vector<ParticleScene*>&) noexcept;
} // namespace neomdl
