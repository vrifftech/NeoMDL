#include <material/Runtime.hpp>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace neomdl::material {
namespace {
float finite(float v, float fallback) { return std::isfinite(v) ? v : fallback; }
float bounded(float v,float lo,float hi,float fallback) { return std::clamp(finite(v,fallback),lo,hi); }
const TxiProperties& props(const TextureBinding& b) {
    static const TxiProperties empty;
    return b.metadata ? b.metadata->txi.properties : empty;
}

bool resolvedTexture(const TextureBinding& binding) {
    return !binding.key.empty() && binding.metadata && binding.metadata->facts.hasPixels;
}

const TextureBinding& k2EnvironmentBinding(const Material& material) {
    // Material::InitializeTextures writes both authored names into one runtime
    // slot; envmaptexture is initialized after bumpyshinytexture and therefore
    // wins even when the resulting texture is invalid.
    return !material.environmentMap.key.empty()
        ? material.environmentMap : material.bumpyShiny;
}

bool k2BumpMap(const Material& material) {
    if (!resolvedTexture(material.bumpMap)) return false;
    const auto& properties = props(material.bumpMap);
    const auto kind = properties.engineBumpMapKind.value_or(
        static_cast<std::int32_t>(properties.bumpMapKind.value_or(BumpMapKind::None)));
    const auto& facts = material.bumpMap.metadata->facts;



    return kind != 0 && (facts.engineProcessed ||
        (kind == 1 && facts.engineBytesPerPixel == 1) ||
        (kind == 2 && facts.engineBytesPerPixel == 4));
}

bool k2BumpyShinyMap(const Material& material) {
    const auto& binding = k2EnvironmentBinding(material);
    return resolvedTexture(binding) && binding.metadata->facts.cubeMap;
}

bool k2EnvironmentMap(const Material& material) {
    return resolvedTexture(k2EnvironmentBinding(material));
}

bool k2Lightmap(const Material& material, bool authoredLightmap) {
    if (!authoredLightmap) return false;
    // Material::InitializeTextures always assigns texture slot 1, using the
    // engine NULL texture for an absent/NULL name. GetRenderPath then clears
    // the authored lightmap bit when AurTextureGetTID(slot 1) is zero.
    return resolvedTexture(material.lightmap);
}

void evaluateBumpFields(RuntimeMaterial& result, const Material& material) {
    const auto& diffuse = props(material.diffuse);
    const auto& bump = props(material.bumpMap);
    if (k2BumpMap(material))
        result.bumpKind = bump.bumpMapKind.value_or(BumpMapKind::Height);
    else
        result.bumpKind = BumpMapKind::None;
    result.bumpScale=bounded(diffuse.bumpMapScaling.value_or(
        bump.bumpMapScaling.value_or(1)),-100,100,1)*
        bounded(bump.bumpIntensity.value_or(diffuse.bumpIntensity.value_or(1)),0,100,1);
    result.diffuseBump=bump.diffuseBump.value_or(diffuse.diffuseBump.value_or(true));
    result.specularBump=bump.specularBump.value_or(diffuse.specularBump.value_or(false));
    result.diffuseBumpIntensity=bounded(bump.diffuseBumpIntensity.value_or(
        diffuse.diffuseBumpIntensity.value_or(1)),0,10,1);
    result.specularBumpIntensity=bounded(bump.specularBumpIntensity.value_or(
        diffuse.specularBumpIntensity.value_or(1)),0,10,1);
    result.specularColor=bump.specularColor.value_or(
        diffuse.specularColor.value_or(std::array<float,3>{{1,1,1}}));
    for(auto& value:result.specularColor) value=bounded(value,0,10,1);
}
}

std::uint32_t k2DesktopExtensionBits(std::string_view extensions) noexcept {
    std::uint32_t result=0;
    // Engine CheckExtension uses space-delimited, case-sensitive whole tokens.
    while(!extensions.empty()) {
        const auto end=extensions.find(' ');
        const auto token=extensions.substr(0,end);
        using namespace k2Extension;
        if(token=="GL_EXT_bgra") result|=Bgra;
        else if(token=="GL_ARB_multitexture") result|=Multitexture;
        else if(token=="GL_NV_register_combiners") result|=RegisterCombiners;
        else if(token=="GL_ARB_texture_cube_map") result|=CubeMap;
        else if(token=="GL_NV_vertex_program") result|=NvVertexProgram;
        else if(token=="GL_NV_texture_shader") result|=NvTextureShader;
        else if(token=="GL_ARB_framebuffer_object") result|=RenderToTexture;
        else if(token=="GL_ATI_text_fragment_shader") result|=AtiFragmentShader;
        else if(token=="GL_ARB_vertex_program") result|=ArbVertexProgram;
        else if(token=="GL_ARB_depth_texture") result|=ArbDepthTexture;
        else if(token=="GL_ARB_fragment_program")
            result|=ArbFragmentProgram|AtiFragmentShader;
        if(end==std::string_view::npos) break;
        extensions.remove_prefix(end+1);
    }
    return result;
}

K2HardwareSelection selectK2HardwareCapabilities(const K2HardwareInputs& inputs) noexcept {
    using namespace k2Extension;
    const auto has=[&](std::uint32_t bits) {
        return (inputs.existingExtensions&bits)==bits;
    };
    K2HardwareSelection result;
    result.arbFragmentPrograms=has(ArbFragmentProgram);
    result.atiValidation=has(AtiFragmentShader) && (inputs.useAtiFragmentShaders&1)!=0;
    result.bumpMapCardValidation=!result.atiValidation;
    result.nonRadeon8500Validation=!result.atiValidation || has(ArbDepthTexture);
    result.multitexture=has(Multitexture);
    result.cubeMap=has(CubeMap|Multitexture);
    result.vertexPrograms=has(ArbVertexProgram) && (inputs.useArbVertexPrograms&1)!=0;
    result.vertexProgramsNv=has(NvVertexProgram);
    constexpr auto common=Bgra|Multitexture|CubeMap|ArbVertexProgram;
    result.bumpMapping=has(common|RegisterCombiners) ||
        (result.vertexPrograms && result.atiValidation);
    result.bumpyShiny=has(common|RegisterCombiners|NvTextureShader) ||
        (has(common|AtiFragmentShader) &&
         (inputs.useArbVertexPrograms&inputs.useAtiFragmentShaders&1)!=0);
    result.nv1xLevelShaders=inputs.renderLevel>0 &&
        !(result.atiValidation && inputs.renderLevel==1) && result.bumpMapping;
    result.nv2xLevelShaders=inputs.renderLevel>=2 && result.bumpyShiny;
    result.atiSoftShadows=inputs.unlockAtiSoftShadows!=0 &&
        has(RenderToTexture|AtiFragmentShader);
    result.atiFrameBufferModifications=has(RenderToTexture|AtiFragmentShader|ArbVertexProgram) &&
        result.nonRadeon8500Validation;
    return result;
}

const char* triMeshRenderPathName(TriMeshRenderPath path) noexcept {
    switch(path) {
    case TriMeshRenderPath::Flat: return "RenderFlat";
    case TriMeshRenderPath::FlatVP: return "RenderFlatVP";
    case TriMeshRenderPath::EMBM: return "RenderEMBM";
    case TriMeshRenderPath::LightMappedBumpSpecular: return "RenderLightMappedBumpSpecular";
    case TriMeshRenderPath::LightMappedBumpDiffuse: return "RenderLightMappedBumpDiffuse";
    case TriMeshRenderPath::BumpMapped: return "RenderBumpMapped";
    case TriMeshRenderPath::BumpmapFallback: return "RenderBumpmapFallback";
    case TriMeshRenderPath::EMLM: return "RenderEMLM";
    case TriMeshRenderPath::EMLMVP: return "RenderEMLMVP";
    case TriMeshRenderPath::NV1xEMLM: return "RenderNV1xEMLM";
    case TriMeshRenderPath::EnvironmentMapped: return "RenderEnvironmentMapped";
    case TriMeshRenderPath::EnvironmentMappedVP: return "RenderEnvironmentMappedVP";
    case TriMeshRenderPath::LightMapped: return "RenderLightMapped";
    case TriMeshRenderPath::LightmappedVP: return "RenderLightmappedVP";
    case TriMeshRenderPath::LightMappedNoShaders: return "RenderLightMappedNoShaders";
    case TriMeshRenderPath::NormalMapped: return "RenderNormalMapped";
    case TriMeshRenderPath::NoiseMapped: return "RenderNoiseMapped";
    }
    return "Unknown PartTriMesh path";
}

bool engineMaterialHasBumpMap(const Material& material) { return k2BumpMap(material); }

bool triMeshRenderPathUsesLightmap(TriMeshRenderPath path) noexcept {
    switch(path) {
    case TriMeshRenderPath::LightMappedBumpSpecular:
    case TriMeshRenderPath::LightMappedBumpDiffuse:
    case TriMeshRenderPath::EMLM:
    case TriMeshRenderPath::EMLMVP:
    case TriMeshRenderPath::NV1xEMLM:
    case TriMeshRenderPath::LightMapped:
    case TriMeshRenderPath::LightmappedVP:
    case TriMeshRenderPath::LightMappedNoShaders:
        return true;
    default:
        return false;
    }
}

bool triMeshRenderPathUsesBumpMap(TriMeshRenderPath path) noexcept {
    switch(path) {
    case TriMeshRenderPath::EMBM:
    case TriMeshRenderPath::LightMappedBumpSpecular:
    case TriMeshRenderPath::LightMappedBumpDiffuse:
    case TriMeshRenderPath::BumpMapped:
    case TriMeshRenderPath::NormalMapped:
        return true;
    default:
        return false;
    }
}

bool triMeshRenderPathUsesEnvironmentMap(TriMeshRenderPath path) noexcept {
    switch(path) {
    case TriMeshRenderPath::EMBM:
    case TriMeshRenderPath::EMLM:
    case TriMeshRenderPath::EMLMVP:
    case TriMeshRenderPath::NV1xEMLM:
    case TriMeshRenderPath::EnvironmentMapped:
    case TriMeshRenderPath::EnvironmentMappedVP:
        return true;
    default:
        return false;
    }
}

EnginePartTriMeshSelection selectEnginePartTriMeshRenderPath(
    const Material& material, const EnginePartTriMeshInputs& inputs,
    EngineMaterialGame game) {
    EnginePartTriMeshSelection result;
    const auto& diffuse = props(material.diffuse);
    result.waterAlpha=diffuse.waterAlpha.value_or(1.0f);
    if(game==EngineMaterialGame::Kotor2)
        result.waterAlpha=finite(result.waterAlpha,1.0f);
    result.bumpMap=k2BumpMap(material);
    // Material::HasNormalMap is a literal false in the K1/K2 builds.
    result.normalMap=false;
    result.bumpyShinyMap=k2BumpyShinyMap(material);
    result.tangentBases=inputs.tangentBases;
    result.environmentMap=k2EnvironmentMap(material) || inputs.ownerEnvironmentMap;
    result.lightmap=k2Lightmap(material,inputs.lightmapped);

    auto hardware=inputs.engineHardware;
    const auto capabilities=selectK2HardwareCapabilities(hardware);
    const bool nv2=inputs.nv2xLevelShaders && capabilities.nv2xLevelShaders;
    if(game==EngineMaterialGame::Kotor1) {
        // K1 Desktop GetRenderPath 0x340820, consumed by Draw 0x34a994.
        // This branch tree does not globally short-circuit wateralpha
        // and uses the non-VP variants.
        using Path=TriMeshRenderPath;
        result.path=Path::Flat;
        if(result.bumpyShinyMap && result.bumpMap && result.tangentBases &&
           inputs.enableBumpyShiny) {
            if(nv2 && capabilities.nonRadeon8500Validation) result.path=Path::EMBM;
            else if(!result.lightmap) result.path=Path::EnvironmentMapped;
            else if(nv2) result.path=Path::EMLM;
            else if(result.waterAlpha<1.0f) result.path=Path::EnvironmentMapped;
            else result.path=capabilities.nv1xLevelShaders
                ? Path::NV1xEMLM : Path::LightMappedNoShaders;
            return result;
        }
        if(result.lightmap && result.bumpMap && result.tangentBases && nv2 &&
           inputs.enableBumpMap && inputs.enableLightmap) {
            // Material+68 is renderbmlmtype, initialized to 1 at 0x32712a.
            result.path=diffuse.renderBmLmType.value_or(1)!=0
                ? Path::LightMappedBumpSpecular : Path::LightMappedBumpDiffuse;
            return result;
        }
        if(result.bumpMap && result.tangentBases && inputs.enableBumpMap && nv2) {
            result.path=capabilities.bumpMapCardValidation && capabilities.nonRadeon8500Validation
                ? Path::BumpMapped : Path::BumpmapFallback;
            return result;
        }
        if(result.normalMap && !result.tangentBases && inputs.enableBumpMap && nv2) {
            result.path=Path::NormalMapped;return result;
        }
        if(result.lightmap && result.environmentMap && inputs.enableLightmap) {
            result.path=nv2 ? Path::EMLM : capabilities.nv1xLevelShaders
                ? Path::NV1xEMLM : Path::LightMappedNoShaders;
            return result;
        }
        if(result.environmentMap) {result.path=Path::EnvironmentMapped;return result;}
        if(result.lightmap && inputs.enableLightmap) {
            result.path=nv2 ? Path::LightMapped : Path::LightMappedNoShaders;
            return result;
        }
        if(result.bumpMap && result.tangentBases && inputs.enableBumpMap && !nv2)
            result.path=Path::BumpmapFallback;
        return result;
    }

    // K2 initializes the default return value to FlatVP.
    if (1.0f > result.waterAlpha) return result;
    if (result.normalMap && inputs.enableBumpMap && !result.tangentBases &&
        nv2) {
        result.path=TriMeshRenderPath::NormalMapped;
        return result;
    }
    if (result.lightmap && result.environmentMap && inputs.enableLightmap) {
        result.path=TriMeshRenderPath::EMLMVP;
        return result;
    }
    if (result.environmentMap) {
        result.path=TriMeshRenderPath::EnvironmentMappedVP;
        return result;
    }
    if (result.lightmap && inputs.enableLightmap) {
        result.path=TriMeshRenderPath::LightmappedVP;
        return result;
    }
    if (!result.bumpMap) return result;
    if (inputs.enableBumpMap && result.tangentBases)
        result.path=TriMeshRenderPath::BumpmapFallback;
    return result;
}
K2PartTriMeshSelection selectK2PartTriMeshRenderPath(
    const Material& material, const K2PartTriMeshInputs& inputs) {
    return selectEnginePartTriMeshRenderPath(material,inputs,EngineMaterialGame::Kotor2);
}
UvState evaluateUv(const TextureBinding& binding) {
    UvState result;
    const auto& p=props(binding);
    result.clamp=p.clamp.value_or(0)!=0;
    // ProceduralAnimateHierarchy writes the shared vertex UV pool. A material
    // clock or texture clamp directive does not produce a separate UV scroll.
    if(!binding.metadata) return result;
    const auto& facts=binding.metadata->facts;
    const auto width=std::max(1u,facts.layerWidth ? facts.layerWidth : facts.width);
    const auto height=std::max(1u,facts.layerHeight ? facts.layerHeight : facts.height);
    result.texel={{1.0f/static_cast<float>(width),1.0f/static_cast<float>(height)}};
    // CycleTIDTextureController owns frame timing and advances at Bind. The
    // texture cache supplies the selected frame image; UV evaluation must not
    // manufacture another frame from an owner's animation/seek time.

    return result;
}
RuntimeMaterial evaluateMaterial(const Material& m,const SurfaceInputs& surface,double) {
    RuntimeMaterial r;
    const auto& p=props(m.diffuse);
    const auto& b=props(m.bumpMap);
    r.opacity=bounded(surface.opacity,0,1,1)*bounded(surface.instanceOpacity,0,1,1)*
        bounded(p.waterAlpha.value_or(1),0,1,1);
    // Display bound, not an authored format range. Preserve the sampled value
    // in Pose; reject non-finite input per component here at the render boundary.
    for(std::size_t channel=0;channel<r.selfIllumination.size();++channel)
        r.selfIllumination[channel]=bounded(surface.selfIllumination[channel],0,16,0);
    const bool environment=(!m.environmentMap.key.empty() || !m.bumpyShiny.key.empty()) &&
        p.environmentMapped.value_or(true);
    // Odyssey-style inverse diffuse-alpha reflection mask. Explicit blend
    // directives take precedence; header alphaBlending is never substituted.
    r.textureAlphaIsOpacity=!environment || p.blending.has_value();
    const auto alpha=m.diffuse.metadata ? m.diffuse.metadata->facts.pixelAlpha : PixelAlpha::Opaque;
    r.alphaTest=p.blending==BlendDirective::PunchThrough ||
        (!p.blending && r.textureAlphaIsOpacity && alpha==PixelAlpha::Packed);
    if(p.blending==BlendDirective::Additive) {
        r.pass=RenderPass::Additive;
        r.blending=true;
        r.blendFunction=BlendFunction::Additive;
        r.depthWrite=false;
    } else if(r.opacity<1 || surface.vertexAlpha || p.decal.value_or(false) ||
        (r.textureAlphaIsOpacity && alpha==PixelAlpha::Fractional && !r.alphaTest)) {
        r.pass=RenderPass::Translucent;
        r.blending=true;
        r.blendFunction=BlendFunction::Alpha;
        r.depthWrite=false;
    } else if(r.alphaTest) {
        r.pass=RenderPass::PunchThrough;
    }
    r.useLightmap=!m.lightmap.key.empty();
    r.useBumpMap=!m.bumpMap.key.empty();
    r.useEnvironmentMap=environment;
    if(!m.bumpMap.key.empty()) r.bumpKind=b.bumpMapKind.value_or(BumpMapKind::Height);
    r.bumpScale=bounded(p.bumpMapScaling.value_or(b.bumpMapScaling.value_or(1)),-100,100,1)*
        bounded(b.bumpIntensity.value_or(p.bumpIntensity.value_or(1)),0,100,1);
    r.diffuseBump=b.diffuseBump.value_or(p.diffuseBump.value_or(true));
    r.specularBump=b.specularBump.value_or(p.specularBump.value_or(false));
    r.diffuseBumpIntensity=bounded(b.diffuseBumpIntensity.value_or(p.diffuseBumpIntensity.value_or(1)),0,10,1);
    r.specularBumpIntensity=bounded(b.specularBumpIntensity.value_or(p.specularBumpIntensity.value_or(1)),0,10,1);
    r.specularColor=b.specularColor.value_or(p.specularColor.value_or(std::array<float,3>{{1,1,1}}));
    for(auto& v:r.specularColor) v=bounded(v,0,10,1);
    r.environmentStrength=environment ? bounded(p.environmentAlpha.value_or(1),0,10,1) : 0;
    r.diffuseUv=evaluateUv(m.diffuse);
    r.bumpUv=evaluateUv(m.bumpMap);
    return r;
}

RuntimeMaterial evaluateEnginePartTriMeshMaterial(
    const Material& material, const SurfaceInputs& surface,
    const EnginePartTriMeshInputs& inputs, EngineMaterialGame game, double) {
    RuntimeMaterial result;
    const auto& diffuse=props(material.diffuse);
    const auto selection=selectEnginePartTriMeshRenderPath(material,inputs,game);
    result.k2PartTriMeshState=true;
    result.engineGame=game;
    result.renderPath=selection.path;
    result.engineWaterAlpha=selection.waterAlpha;
    result.useLightmap=triMeshRenderPathUsesLightmap(selection.path);
    result.useBumpMap=triMeshRenderPathUsesBumpMap(selection.path);
    result.useEnvironmentMap=triMeshRenderPathUsesEnvironmentMap(selection.path);
    auto hardware=inputs.engineHardware;
    result.engineHardware=selectK2HardwareCapabilities(hardware);
    result.engineAtiValidation=result.engineHardware.atiValidation;
    result.engineMaterialEnableBumpyShiny=inputs.enableBumpyShiny;
    result.engineSkinBumpTangent=selection.bumpMap && selection.tangentBases;
    // RenderSkinnedVP tests bumpy-shiny first and does not use enablebumpmap,
    // renderlevel or AurBumpMapCardValidate for that branch. The following
    // plain bump branches do require all three (and the tangent basis).
    result.skinShinyAvailable=selection.bumpMap && selection.tangentBases &&
        selection.bumpyShinyMap && inputs.enableBumpyShiny && result.engineHardware.bumpyShiny;
    result.skinBumpAvailable=selection.bumpMap && selection.tangentBases &&
        inputs.enableBumpMap && inputs.nv2xLevelShaders &&
        result.engineHardware.nv2xLevelShaders && result.engineHardware.bumpMapCardValidation;
    result.engineSkinBumpFallback=selection.bumpMap && selection.tangentBases &&
        (!result.engineHardware.nv2xLevelShaders || !result.engineHardware.bumpMapCardValidation);
    result.skinEnvironmentAvailable=selection.environmentMap;
    result.engineMaterialEnvironmentAvailable=k2EnvironmentMap(material);
    if(game==EngineMaterialGame::Kotor1 &&
       selection.path==TriMeshRenderPath::LightMappedNoShaders)
        result.useEnvironmentMap=result.engineMaterialEnvironmentAvailable;
    if(surface.skinned) {
        result.useLightmap=false;
        result.useBumpMap=result.skinBumpAvailable || result.skinShinyAvailable;
        result.useEnvironmentMap=result.skinShinyAvailable || result.skinEnvironmentAvailable;
    }


    // Engine material arithmetic is not clamped here. Vertex primary color
    // is clamped after lighting, before interpolation; fragment env alpha is
    // a separate, unclamped constant. Non-finite inputs fail at submission.
    result.instanceOpacity=surface.instanceOpacity;
    result.environmentOpacity=surface.opacity;
    result.vertexOpacity=surface.opacity*surface.instanceOpacity;
    result.fixedFunctionAlphaScale=diffuse.useGlobalAlpha.value_or(false)
        ? surface.globalOpacity : 1.0f;
    result.opacity=result.vertexOpacity*result.fixedFunctionAlphaScale;

    const bool decal=diffuse.decal.value_or(false);
    for(std::size_t channel=0;channel<result.selfIllumination.size();++channel) {
        result.selfIllumination[channel]=decal ? 1.0f : surface.selfIllumination[channel];
    }

    // Material::SetBlendingMode(0): all K2 PartTriMesh materials use one of
    // these exact factor pairs. Alpha testing is active under the supplied
    // build's usenewalpha path, with a strict greater-than test. GL_BLEND
    // ownership belongs to Scene buckets, so the viewer enables it only for
    // submissions that actually require compositing.
    result.depthWrite=true;
    result.alphaTest=true;
    result.alphaCutoff=0.0f;
    result.blendFunction=BlendFunction::Alpha;
    if(diffuse.blending==BlendDirective::Additive) {
        result.pass=RenderPass::Additive;
        result.blendFunction=BlendFunction::Additive;
        result.depthWrite=false;
    } else if(diffuse.blending==BlendDirective::PunchThrough) {
        result.pass=RenderPass::PunchThrough;
        result.blendFunction=BlendFunction::Replace;
        result.alphaCutoff=0.35f;
    }

    result.engineMaterialBlendFunction=result.blendFunction;
    const bool environment=result.useEnvironmentMap;
    result.textureAlphaIsOpacity=!environment || diffuse.blending.has_value();
    const auto alpha=material.diffuse.metadata
        ? material.diffuse.metadata->facts.pixelAlpha : PixelAlpha::Opaque;
    if(result.pass==RenderPass::Opaque &&
       (result.opacity<1.0f || surface.vertexAlpha ||
        (result.textureAlphaIsOpacity && alpha==PixelAlpha::Fractional))) {
        // Bucket selection is still a scene policy; this does not alter the
        // Material blend factors or depth-write state.
        result.pass=RenderPass::Translucent;
    }

    // The selected K2 environment renderers disable GL_ALPHA_TEST around the
    // draw. RenderBumpmapFallback does the same and additionally switches to
    // GL_ONE/GL_ZERO while delegating to RenderFlat. None of these paths alters
    // the current depth mask.
    if(!surface.skinned && (selection.path==TriMeshRenderPath::EnvironmentMappedVP ||
       selection.path==TriMeshRenderPath::EMLMVP ||
       selection.path==TriMeshRenderPath::BumpmapFallback)) {
        result.alphaTest=false;
    }
    if(!surface.skinned && selection.path==TriMeshRenderPath::BumpmapFallback)
        result.blendFunction=BlendFunction::Replace;
    result.blending=result.pass==RenderPass::Translucent ||
        result.pass==RenderPass::Additive;

    evaluateBumpFields(result,material);
    // CAurTextureBasic constructor 0xbb837 / 0xbb94f..0xbb95d; getters
    // 0xbacf0 / 0xbaca4. Non-finite authored data fails at draw validation.
    result.engineBumpIntensity=props(material.bumpMap).bumpIntensity.value_or(1.0f);
    // K1 CAurTextureBasic initializes both authored bump flags and both
    // intensities to one (0x3709df..0x3709e2, 0x370aa4..0x370aab).
    // The getters return zero when the corresponding bump flag is disabled.
    result.engineDiffuseBump=selection.bumpMap && props(material.bumpMap).diffuseBump.value_or(true);
    result.engineSpecularBump=selection.bumpMap && props(material.bumpMap).specularBump.value_or(true);
    result.engineDiffuseBumpIntensity=result.engineDiffuseBump
        ? props(material.bumpMap).diffuseBumpIntensity.value_or(1.0f) : 0.0f;
    result.engineSpecularBumpIntensity=result.engineSpecularBump
        ? props(material.bumpMap).specularBumpIntensity.value_or(1.0f) : 0.0f;
    result.engineSpecularColor=props(material.bumpMap).specularColor.value_or(
        std::array<float,3>{{1,1,1}});
    if(!result.useBumpMap) result.bumpKind=BumpMapKind::None;
    // EnvFP/EMLMFP use (1-base.a) directly. TXI environmental alpha is not
    // an extra reflection gain in these selected engine programs.
    result.environmentStrength=result.useEnvironmentMap ? 1.0f : 0.0f;
    result.diffuseUv=evaluateUv(material.diffuse);
    result.bumpUv=evaluateUv(material.bumpMap);
    return result;
}
RuntimeMaterial evaluateK2PartTriMeshMaterial(
    const Material& material, const SurfaceInputs& surface,
    const K2PartTriMeshInputs& inputs, double seconds) {
    return evaluateEnginePartTriMeshMaterial(material,surface,inputs,EngineMaterialGame::Kotor2,seconds);
}
std::vector<std::size_t> renderQueueOrder(const std::vector<QueueKey>& keys) {
    std::vector<std::size_t> order(keys.size());std::iota(order.begin(),order.end(),0u);
    std::stable_sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b) {
        const auto& x=keys[a];const auto& y=keys[b];
        if(x.pass!=y.pass) return x.pass<y.pass;
        if(x.pass==RenderPass::Translucent) {
            const float xd=finite(x.eyeDepth,0),yd=finite(y.eyeDepth,0);
            if(xd!=yd) return xd>yd;
            // A complete tie-break is required for strict weak ordering. The
            // engine-defined part is the signed renderOrder comparison within
            // one nonzero owner group. Group ordering (including ordinary
            // meshes in group zero) is only a deterministic equal-depth tie.
            if(x.authoredOrderGroup!=y.authoredOrderGroup)
                return x.authoredOrderGroup<y.authoredOrderGroup;
            if(x.authoredOrderGroup!=0 && x.authoredOrder!=y.authoredOrder)
                return x.authoredOrder<y.authoredOrder;
        }
        return false;
    });
    return order;
}
} // namespace neomdl::material
