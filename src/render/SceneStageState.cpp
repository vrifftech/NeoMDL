// Scene::RenderSinglePass: K1 Desktop 0x356de0; K2 Desktop 0xc6026.
// These are caller transitions, not host-GL snapshots or new effect algorithms.
#include "render/SceneStageState.hpp"
#include <algorithm>

namespace neomdl::render {
namespace {
using Stage=model::K2SceneStage;
void ordinaryBlend(EngineSceneRasterState& s) {
    s.blend=true;s.source=gl::SrcAlpha;s.destination=gl::OneMinusSrcAlpha;
}
void clearStencil(int bits,EngineSceneRasterState& s,EngineSceneStageActions& a) {
    // SetShadowStencilBuffer does not write the clear value without stencil.
    if(bits>0)s.stencilClear=engineSceneStencilClearValue(bits);
    s.stencilWriteMask=0xff;
    a.clearStencil=true;
}
}
gl::Int engineSceneStencilClearValue(int bits) noexcept {
    return bits<=0?0:static_cast<gl::Int>((gl::UInt{1}<<(std::min(bits,32)-1))-1);
}
EngineSceneStageActions enterEngineSceneStage(Stage stage,const EngineSceneSettings& cfg,
        int bits,EngineSceneRasterState& s) {
    EngineSceneStageActions a;
    const bool k1=cfg.game==model::GameVersion::Kotor1;
    switch(stage) {
    case Stage::FogSetup:
        if(k1) {
            // Scene::Render 0x35699e..0x356a0c precedes RenderSinglePass.
            // Do not inherit a previous game's late-frame alpha reference.
            s.depthFunction=gl::Lequal;s.depthTest=true;s.blend=false;s.cullFaces=true;s.alphaTest=true;s.lighting=true;
            s.alphaFunction=gl::Greater;s.alphaReference=cfg.sceneAlphaReference;
            s.clientArrays[0]=s.clientArrays[1]=s.clientArrays[3]=true;
        }
        s.fog=cfg.fogEnabled;break;
    case Stage::StaticMeshGeometry:
        // RenderSinglePass, after fog setup and before RenderStaticGeometry.
        ordinaryBlend(s);s.stencilTest=false;s.colorMaterial=false;s.clientArrays[2]=false;
        s.globalAmbient={{0,0,0,1}};
        // RenderStaticGeometry: ambient/diffuse white, EMISSION black.
        // The third material enum is 0x1600, not the specular enum.
        s.materialAmbient=s.materialDiffuse={{1,1,1,1}};
        s.materialEmission={{0,0,0,1}};
        s.depthWrite=true;
        break;
    case Stage::TriggerGeometry:
        if(cfg.triggersEnabled)clearStencil(bits,s,a);
        break;
    case Stage::TriggerHighlightBeforeDynamic:
    case Stage::TriggerHighlightAfterDynamic:
        if(cfg.triggersEnabled) {
            if(stage==Stage::TriggerHighlightAfterDynamic)clearStencil(bits,s,a);
            // DoHighlightTriggers returns before these writes without stencil.
            if(bits>0) {s.blend=false;s.texture2D=false;s.lighting=false;s.alphaTest=false;}
        }
        break;
    case Stage::Shadows:
        // Unconditional, even with shadows off and an empty caster list.
        clearStencil(bits,s,a);
        if(k1 && cfg.shadowsEnabled && cfg.worldEffectsEnabled && bits>0) {
            // RenderShadows 0x35585b..0x355894, before light admission.
            s.texture2D=false;s.alphaTest=false;s.lighting=false;s.stencilTest=true;
        }
        break;
    case Stage::DynamicGobs:
        // Desktop 0xc4890: NEVER undo this at bucket exit.
        if(cfg.shadowsEnabled) {
            s.stencilTest=true;s.stencilWriteMask=0xff;
            s.stencilFunction=gl::Always;s.stencilReference=engineSceneStencilClearValue(bits);
            s.stencilReadMask=~0u;s.stencilFail=s.stencilDepthFail=gl::Keep;
            s.stencilDepthPass=gl::Replace;
        }
        break;
    case Stage::PersonalSpace:
        // RenderPersonalSpace 0x3550b5..0x3550c0 precedes owner admission.
        if(k1)s.depthTest=false;
        break;
    case Stage::Paths:
        if(cfg.pathsEnabled) {
            // The renderpaths gate precedes these writes in both games:
            // K1 0x355219, K2 0xc5b00. Empty lists execute the wrapper.
            s.depthTest=false;s.lighting=false;s.texture2D=false;
        }
        break;
    case Stage::Distortion:
        if(k1 || cfg.effects.enabled) {
            // DoDistortionBuckets 0xc218e. Deliberately no stencil clear.
            s.stencilTest=true;s.colorWrite.fill(false);s.stencilWriteMask=1;
            s.stencilFunction=gl::Always;s.stencilReference=1;s.stencilReadMask=0xff;
            s.stencilFail=s.stencilDepthFail=gl::Keep;s.stencilDepthPass=gl::Replace;
        }
        break;
    case Stage::Emitters:
        // Both DoEmitterBucket bodies clear before enableemitters:
        // K1 0x356004, K2 0xc3e6d. Empty buckets still start/stop.
        s.stencilClear=0;s.stencilWriteMask=0xff;a.clearStencil=true;
        if(cfg.emittersEnabled) {
            s.stencilTest=false;s.depthWrite=false;
            s.lighting=false;s.blend=true;s.alphaTest=false;
            s.clientArrays={{true,false,true,true}};
        }
        break;
    case Stage::Lightsabers:
        // DoLightSaberBucket 0x354b89..0x354b93, including an empty list.
        // K2 0xc2287 makes the same write.
        s.blend=true;
        break;
    case Stage::DrawableSquares:
        // Both helpers write even for an empty list (K1 0x2fd2ef, K2 0x8ef0d).
        s.lighting=false;s.texture2D=false;s.depthTest=false;
        break;
    case Stage::MotionBlur:
        ordinaryBlend(s);s.depthTest=true;s.lighting=true;
        s.clientArrays={{true,true,true,true}};
        s.depthWrite=false;
        break;
    case Stage::FramebufferEffects:
    case Stage::LensFlares:
        // RenderSinglePass owns glAlphaFunc(GREATER,0), NOT the flare caller.
        // K1 has no matching late alpha reset (0x3572aa..0x3572f4).
        if(!k1) {s.alphaFunction=gl::Greater;s.alphaReference=0;}
        if(stage==Stage::FramebufferEffects && cfg.worldEffectsEnabled) {
            // K1 FrameBufferModifications 0x311444..0x31144c and K2 Desktop
            // 0x8f8e9 disable fog before the capability/ATI body checks.
            s.fog=false;
        }
        break;
    case Stage::FramebufferEffects2:
        if(cfg.worldEffectsEnabled) {s.depthTest=false;s.lighting=false;}
        break;
    default:break;
    }
    return a;
}
EngineSceneStageActions leaveEngineSceneStage(Stage stage,const EngineSceneSettings& cfg,
        int bits,const EngineSceneStageResult& result,EngineSceneRasterState& s) {
    EngineSceneStageActions a;
    const bool k1=cfg.game==model::GameVersion::Kotor1;
    switch(stage) {
    case Stage::StaticTriangleBins:
        // The static wrapper contains BOTH DoMeshBuckets and DoBinBucket.
        s.globalAmbient={{cfg.sceneAmbient[0],cfg.sceneAmbient[1],cfg.sceneAmbient[2],1}};
        s.colorMaterial=false;s.clientArrays[2]=false;
        break;
    case Stage::Shadows:
        if(k1 && cfg.shadowsEnabled && cfg.worldEffectsEnabled && bits>0) {
            // RenderShadows 0x355a75..0x355ae6 also runs for an empty list.
            s.clientArrays[0]=s.clientArrays[1]=true;s.depthWrite=true;
            s.texture2D=true;s.alphaTest=true;s.lighting=true;s.stencilTest=false;
        }
        break;
    case Stage::TriggerHighlightBeforeDynamic:
    case Stage::TriggerHighlightAfterDynamic:
        if(cfg.triggersEnabled && bits>0) {
            if(result.highlightVolumeDrawn) {
                s.stencilTest=false;s.depthTest=true;ordinaryBlend(s);
                s.depthWrite=true;
            }
            s.alphaTest=true;s.blend=true;s.texture2D=true;s.lighting=true;
        }
        break;
    case Stage::MotionBlur:
        s.depthWrite=true;
        break;
    case Stage::Emitters:
        if(cfg.emittersEnabled) {
            // StopEmitterLayer retains alpha-test state.
            s.depthWrite=true;s.lighting=true;s.clientArrays[1]=true;
            s.source=gl::SrcAlpha;s.destination=gl::OneMinusSrcAlpha;
        }
        break;
    case Stage::DrawableSquares:
        // 0x2fd332..0x2fd345: texturing is deliberately not restored.
        s.depthTest=true;s.lighting=true;
        break;
    case Stage::PersonalSpace:
        if(k1)s.depthTest=true;
        break;
    case Stage::Paths:
        if(cfg.pathsEnabled) {
            s.lighting=true;s.texture2D=true;s.depthTest=true;
        }
        break;
    case Stage::Distortion:
        if(k1 || cfg.effects.enabled) {
            s.colorWrite.fill(true);s.stencilWriteMask=0xff;s.stencilTest=false;
        }
        break;
    case Stage::FramebufferEffects:
    case Stage::FramebufferEffects2:
        // ATI2 (K1, K2 Desktop 0x95720) returns before setup/cleanup unless the saturation pass is
        // enabled. RenderSinglePass's preceding depth/lighting disable remains.
        if(cfg.effects.enabled &&
           (!(k1||!cfg.effects.desktopArbShaders) || stage==Stage::FramebufferEffects || cfg.effects.saturationEnabled)) {
            ordinaryBlend(s);s.depthTest=true;s.lighting=true;s.depthWrite=true;
            // K2 ATI restores alpha as well (0x90b35 / 0x958e5).
            if(k1||!cfg.effects.desktopArbShaders)s.alphaTest=true;
            if(!k1&&!cfg.effects.desktopArbShaders && stage==Stage::FramebufferEffects) {
                // ATI entry disables blend (0x90308). Only these helpers
                // re-enable it; the final 0x90b28 writes factors, not enable.
                s.blend=cfg.effects.desktopConvolution||cfg.distortionProducerCount||
                    cfg.effects.dream||cfg.effects.furyLevel;
            }
            if(stage==Stage::FramebufferEffects && cfg.distortionProducerCount) {
                s.stencilFunction=0x0201; // GL_LESS
                s.stencilReference=0;s.stencilReadMask=1;
                s.stencilFail=s.stencilDepthFail=s.stencilDepthPass=gl::Keep;
            }
            s.stencilTest=false;
        }
        break;
    case Stage::LensFlares:
        if(result.lensFlareCallerRan) {
            ordinaryBlend(s);s.depthTest=true;s.lighting=true;s.texture2D=true;
            if(cfg.fogEnabled)s.fog=true;
        }
        break;
    default:break;
    }
    return a;
}
void finishEngineScene(EngineSceneRasterState& s) noexcept {
    s.stencilTest=false;s.lighting=false;s.alphaTest=true;
    s.depthTest=true;s.blend=true;s.fog=false;s.colorMaterial=false;s.clientArrays.fill(false);
}
void applyEngineSceneRasterState(gl::Api& a,const EngineSceneRasterState& s) {
    const auto cap=[&](gl::Enum c,bool enabled) {if(enabled)a.Enable(c);else a.Disable(c);};
    cap(gl::Blend,s.blend);cap(gl::DepthTest,s.depthTest);cap(gl::AlphaTest,s.alphaTest);
    cap(gl::Lighting,s.lighting);cap(gl::Fog,s.fog);cap(gl::StencilTest,s.stencilTest);
    cap(gl::ColorMaterial,s.colorMaterial);cap(gl::CullFace,s.cullFaces);
    a.LightModelfv(gl::LightModelAmbient,s.globalAmbient.data());
    a.Materialfv(gl::Front,gl::MaterialAmbient,s.materialAmbient.data());
    a.Materialfv(gl::Front,gl::MaterialDiffuse,s.materialDiffuse.data());
    a.Materialfv(gl::Front,gl::MaterialEmission,s.materialEmission.data());
    // The engine caller addresses the diffuse unit, not the host's active unit.
    a.ActiveTexture(gl::Texture0);cap(gl::Texture2D,s.texture2D);
    a.BlendFuncSeparate(s.source,s.destination,s.source,s.destination);
    a.BlendEquationSeparate(s.blendEquationRgb,s.blendEquationAlpha);
    a.ColorMask(s.colorWrite[0],s.colorWrite[1],s.colorWrite[2],s.colorWrite[3]);
    a.DepthFunc(s.depthFunction);a.DepthMask(s.depthWrite?1:0);a.AlphaFunc(s.alphaFunction,s.alphaReference);
    a.StencilFuncSeparate(gl::FrontAndBack,s.stencilFunction,s.stencilReference,s.stencilReadMask);
    a.StencilMaskSeparate(gl::FrontAndBack,s.stencilWriteMask);
    a.StencilOpSeparate(gl::FrontAndBack,s.stencilFail,s.stencilDepthFail,s.stencilDepthPass);
    a.ClearStencil(s.stencilClear);
}
} // namespace neomdl::render
