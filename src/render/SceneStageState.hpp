#pragma once
#include "render/GlApi.hpp"
#include "render/ScenePostprocess.hpp"
#include <model/Lighting.hpp>
#include <model/Model.hpp>
#include <model/SceneOrdering.hpp>
#include <array>

namespace neomdl::render {
// Engine register image, never a snapshot of the embedding application's GL.
// The constructor is the viewer's explicit initial scene state; subsequent
// frames retain it. Only caller writes are changed by transitions.
struct EngineSceneRasterState {
    bool blend{true},depthTest{true},depthWrite{true},alphaTest{true};
    bool lighting{true},fog{},texture2D{true},stencilTest{},cullFaces{true};
    gl::Enum source{gl::SrcAlpha},destination{gl::OneMinusSrcAlpha};
    // Explicit viewer/engine entry state, not the host's incoming comparison
    // or blend equation. The stage wrappers do not modify these.
    gl::Enum depthFunction{gl::Lequal},blendEquationRgb{gl::FuncAdd},blendEquationAlpha{gl::FuncAdd};
    gl::Enum alphaFunction{gl::Greater};float alphaReference{};
    gl::Enum stencilFunction{gl::Always};gl::Int stencilReference{};
    gl::UInt stencilReadMask{~0u},stencilWriteMask{~0u};
    gl::Enum stencilFail{gl::Keep},stencilDepthFail{gl::Keep},stencilDepthPass{gl::Keep};
    gl::Int stencilClear{};
    std::array<bool,4> colorWrite{{true,true,true,true}};
    bool colorMaterial{};
    std::array<float,4> globalAmbient{{0,0,0,1}};
    std::array<float,4> materialAmbient{{.2f,.2f,.2f,1}};
    std::array<float,4> materialDiffuse{{.8f,.8f,.8f,1}};
    std::array<float,4> materialEmission{{0,0,0,1}};
    // Engine conventional arrays (vertex,normal,color,uv0). Shader consumers
    // map these to their own generic attribute slots; do not enable unrelated
    // host conventional arrays as a side effect of executing a shader bucket.
    std::array<bool,4> clientArrays{};
};
struct EngineSceneSettings {
    model::GameVersion game{model::GameVersion::Kotor2};
    bool shadowsEnabled{},triggersEnabled{},fogEnabled{};
    // Scene fog parameters consumed by K1 ATI framebuffer programs. Their
    // quads retain fixed fog when switching the same context's pbuffer drawable.
    model::FogSettings sceneFog;
    // Scene::Render's engine global alphatest. K1 resets the comparison here
    // and retains subsequent material writes through the late-frame stages.
    float sceneAlphaReference{};
    bool emittersEnabled{true};
    bool pathsEnabled{};
    bool grassEnabled{true}; // DoBinBucket's enablegrass, including empty-bin calls
    // RenderSinglePass's scene +0xe0 (K2) / +0xd4 (K1) guard. Full passes set it;
    // the scene-effect graph cannot be entered through the Gob-only path.
    bool worldEffectsEnabled{true};
    std::array<float,3> sceneAmbient{{.25f,.25f,.25f}};
    SceneEffectsSettings effects;
    // Number of admitted owners, not triangle/mesh count. Hologram admission
    // takes precedence. An admitted owner must be drawn only in this bucket.
    std::size_t distortionProducerCount{};
    SceneEffectTextureBinding bindEffectTexture;

};
struct EngineSceneStageResult {
    // The engine highlight caller's per-volume exit differs from an empty
    // bucket. Do not disable dynamic stencil merely because it was invoked.
    bool highlightVolumeDrawn{};
    bool lensFlareCallerRan{};
};
struct EngineSceneStageActions {
    bool clearStencil{};
};
// AurGetStencilClearValue, shared by SetShadowStencilBuffer and dynamic Gobs.
gl::Int engineSceneStencilClearValue(int stencilBits) noexcept;
EngineSceneStageActions enterEngineSceneStage(model::K2SceneStage,
    const EngineSceneSettings&,int stencilBits,EngineSceneRasterState&);
EngineSceneStageActions leaveEngineSceneStage(model::K2SceneStage,
    const EngineSceneSettings&,int stencilBits,const EngineSceneStageResult&,
    EngineSceneRasterState&);
void finishEngineScene(EngineSceneRasterState&) noexcept;
// Apply logical caller state around a bucket. Individual mesh draws restore
// this state; only the outer scene guard restores the application's state.
void applyEngineSceneRasterState(gl::Api&,const EngineSceneRasterState&);
} // namespace neomdl::render
