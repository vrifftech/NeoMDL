#include "render/OwnerEffects.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace neomdl::render {
namespace {
template<class T> bool finite(const T& values) {
    return std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);});
}
bool finite(model::Vec3 v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
const neoshared::material::TxiProperties& properties(const neoshared::material::Material& m) {
    static const neoshared::material::TxiProperties empty;
    return m.diffuse.metadata ? m.diffuse.metadata->txi.properties : empty;
}
}

bool validOwnerVisualEffects(const OwnerVisualEffects& e,std::string& error) {
    error.clear();
    if(!finite(e.colorShift)||!finite(e.illumination)||!finite(e.perObjectIllumination)||
       !std::isfinite(e.shellAmount)||!std::isfinite(e.instanceOpacity)) {
        error="Owner effect values must be finite"; return false;
    }
    if(e.instanceOpacity<0.0f||e.instanceOpacity>1.0f) {
        error="Owner instance opacity must be within [0,1]"; return false;
    }
    if(!e.environmentMap.empty() && neoshared::material::textureResourceKey(e.environmentMap).empty()) {
        error="Owner environment-map override is not a valid ResRef"; return false;
    }
    if(e.shellAmount!=0.0f && neoshared::material::textureResourceKey(e.shellTexture).empty()) {
        error="A bumped-out shell requires a texture ResRef"; return false;
    }
    return true;
}
bool hologramTriMeshPathDraws(material::TriMeshRenderPath path) noexcept {
    // Desktop RenderHologramEffect 0xd483d/table 0x5439c8.
    switch(path) {
    case material::TriMeshRenderPath::Flat:
    case material::TriMeshRenderPath::FlatVP:
    case material::TriMeshRenderPath::EMBM:
    case material::TriMeshRenderPath::LightMappedBumpSpecular:
    case material::TriMeshRenderPath::LightMappedBumpDiffuse:
    case material::TriMeshRenderPath::BumpMapped:
    case material::TriMeshRenderPath::BumpmapFallback:
    case material::TriMeshRenderPath::EnvironmentMapped:
    case material::TriMeshRenderPath::EnvironmentMappedVP:
    case material::TriMeshRenderPath::NormalMapped: return true;
    default: return false;
    }
}
material::RuntimeMaterial shellMaterial(const neoshared::material::TxiMetadata& txi) {
    material::RuntimeMaterial result;
    result.k2PartTriMeshState=true;
    result.blending=true; // Scene's active blend scope, not SetBlendingMode itself
    result.depthWrite=true;
    result.alphaTest=true;
    result.alphaCutoff=0.0f;
    result.pass=material::RenderPass::Translucent;
    result.blendFunction=material::BlendFunction::Alpha;
    // Desktop Gob::EnableRenderBumpedOut 0xab1c6:
    // default 0/1; recognized directives overwrite; unknown values do not.
    for(const auto& item:txi.entries) {
        const auto& entry=item.source;
        if(entry.blankOrComment||entry.listData||entry.listTerminator) continue;
        std::string key=entry.key;
        std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(key!="blending") continue;
        // Parse(char*,char*) at Desktop 0xe5858 uses sscanf("%s"); trailing
        // words are not part of the case-sensitive comparison.
        const auto first=entry.value.find_first_not_of(" \t\r\n\v\f");
        const auto last=entry.value.find_first_of(" \t\r\n\v\f",first);
        const auto value=first==std::string::npos ? std::string{} : entry.value.substr(first,last-first);
        if(value=="additive") {
            result.blendFunction=material::BlendFunction::Additive;
            result.pass=material::RenderPass::Additive;
            result.depthWrite=false; result.alphaCutoff=0.0f;
        } else if(value=="punchthrough") {
            result.blendFunction=material::BlendFunction::Replace;
            result.pass=material::RenderPass::PunchThrough;
            result.depthWrite=true; result.alphaCutoff=0.35f;
        }
    }
    return result;
}

void applyEngineDoorAlpha(MeshDrawParameters& p,float alpha) noexcept {
    p.engineDoorAlpha=alpha;
    // Gob::PartDraw calls Mat before K1 PartLightsaber::Draw, but the latter
    // supplies white current color (0x342b8f) when its lighting is disabled.
    if(p.engineLightsaberMaterial && !p.lighting)return;
    if(p.skinEffect==SkinRenderEffect::BumpedOutShell && !p.engineCpuSkin)return;
    // cSkinMotionBlurVP (K1 0x48d358) writes COL0 from c16/c69, not
    // Mat/env94. Keep the factor for a following CPU shell, but do not tint
    // this shader's independently produced black/half-alpha output.
    if(p.skinEffect==SkinRenderEffect::MotionBlur)return;
    // K1 0x3f83fc..0x3f8414; K2 PartTriMesh::Mat 0xdb469..0xdb482.
    // The owner multiplier follows color shifting and precedes all Mat
    // consumers: fixed diffuse alpha, env94, and raw skinned VP constants.
    if(p.hasColorOverride)p.colorOverride[3]*=alpha;
    p.material.environmentOpacity*=alpha;
    p.material.vertexOpacity*=alpha;
    p.material.opacity*=alpha;
}

bool configureOwnerVisualEffects(const OwnerVisualEffects& e,const OwnerEffectTextures& textures,
    const neoshared::material::Material& authored,const model::NodeMaterialState& state,
    MeshSubmission& submission,std::string& error) {
    if(!validOwnerVisualEffects(e,error)) return false;
    if(!submission.mesh) {error="Owner effect submission has no mesh";return false;}
    auto next=submission;
    const auto& mesh=*next.mesh;
    auto& p=next.parameters;
    if(!p.material.k2PartTriMeshState) {
        error="Owner visual effects require a selected engine material runtime";
        return false;
    }
    const bool k2=p.material.engineGame==material::EngineMaterialGame::Kotor2;
    p.forceSightOwner=e.forceSight;
    p.forceSightIllumination=e.perObjectIlluminationEnabled;
    p.ownerIllumination=e.perObjectIllumination;
    next.trailingShell.reset();
    next.drawPrimary=true;
    p.ownerFragment=OwnerFragmentEffect::None;
    const auto& props=properties(authored);
    // RenderSkinnedVP 0x1097e0 / 0x109942 resolves capability gates before
    // hologram interception. Reuse that evaluated caller state so the ATI
    // branch does not acquire the rejected ordinary-bump alpha policy.
    const bool specializedSkinBump=mesh.skinned && p.engineSkinVertexPrograms &&
        (p.material.skinShinyAvailable || p.material.skinBumpAvailable);
    // PartTriMesh::Mat 0xdb39d..0xdb4dd. Do not fold EnableIllumination into
    // this state: its fragment program has a separate owner color and alpha.
    // K1 DynamicPartCalculateMaterial 0x3f83a0 shares color shift and additive
    // illumination; the later K2 per-object fragment effect is independent.
    if(e.colorShiftEnabled || e.illuminationEnabled || (k2&&e.perObjectIlluminationEnabled)) {
        p.hasColorOverride=true;
        p.colorOverride={{mesh.diffuse.x,mesh.diffuse.y,mesh.diffuse.z,state.alpha}};
        if(e.colorShiftEnabled) for(std::size_t c=0;c<4;++c) p.colorOverride[c]*=e.colorShift[c];
        p.material.environmentOpacity=p.colorOverride[3];
        p.material.vertexOpacity=p.colorOverride[3]*p.material.instanceOpacity;
        p.material.opacity=p.material.vertexOpacity*p.material.fixedFunctionAlphaScale;
        for(std::size_t c=0;c<3;++c) {
            p.material.selfIllumination[c]=props.decal.value_or(false) ? 1.0f : state.selfIllumination[c];
            if(e.illuminationEnabled) p.material.selfIllumination[c]+=e.illumination[c];
        }
        if(p.material.opacity<1.0f && p.material.pass==material::RenderPass::Opaque)
            p.material.pass=material::RenderPass::Translucent;
        // Engine material factors/depth-write are unchanged by Gob alpha.
        p.material.blending=true;
    }
    // GobOrderCmp reads this stored member independently of its enable flag.
    next.ownerAlpha=e.colorShift[3];
    if(k2 && e.hologram && !mesh.animated && (!mesh.skinned || p.engineSkinVertexPrograms)) {
        const bool excluded=mesh.k2Material && mesh.k2Material->hologramDoNotDrawRaw!=0;
        next.drawPrimary=!excluded && (mesh.skinned || hologramTriMeshPathDraws(p.material.renderPath));
        if(next.drawPrimary) {
            if(!textures.hologramFilter || !p.diffuseTexture) {
                error="Hologram drawing requires the resolved diffuse texture and scene Holotex"; return false;
            }
            if(mesh.skinned && (!p.exactK2SkinVertexProgram || !next.deformed || !next.deformed->valid)) {
                error="Hologram skin requires the evaluated K2 skin position pool"; return false;
            }
            p.ownerFragment=OwnerFragmentEffect::Hologram;
            p.hologramFilterTexture=textures.hologramFilter;
            p.material.blending=true;
            // Hologram interception precedes the ordinary path's alpha-test
            // disable. Retain Mat's depth state and strict alpha comparison.
            // RenderSkinnedVP 0x109854/0x1099a1 disables alpha testing in
            // bumpy-shiny and specular-bump branches BEFORE their Holo shader.
            // Plain/environment skins retain Mat's comparison instead.
            p.material.alphaTest=!specializedSkinBump;
            p.material.alphaCutoff=props.blending==neoshared::material::BlendDirective::PunchThrough ? 0.35f : 0.0f;
        }
    } else if(k2 && e.perObjectIlluminationEnabled && !mesh.animated) {
        using Path=material::TriMeshRenderPath;
        const auto path=p.material.renderPath;
        if(path==Path::Flat || path==Path::EnvironmentMapped) {
            error="Owner illumination's legacy fixed-function vertex state is not the translated ARB vertex-program path";
            return false;
        }
        if(!mesh.skinned && path==Path::FlatVP) {
            p.ownerFragment=OwnerFragmentEffect::Illumination;
        } else if(!specializedSkinBump &&
                  (path==Path::EnvironmentMappedVP ||
                   (mesh.skinned && !p.engineSkinVertexPrograms && p.material.skinEnvironmentAvailable))) {
            // RenderEnvironmentMapped's cube/ARB consumer. No synthetic 2D
            // environment lookup and no activation of unrelated skin bump paths.
            if(!p.environmentTexture || !p.environmentCube) {
                error="The engine per-object environment illumination path requires a cubemap";
                return false;
            }
            p.ownerFragment=OwnerFragmentEffect::IlluminationEnvironment;
        }
        if(p.ownerFragment!=OwnerFragmentEffect::None) {
            if(!p.diffuseTexture) {error="Per-object illumination requires the resolved diffuse texture";return false;}
            p.ownerIllumination=e.perObjectIllumination;
        }
        // Plain VP skins, CPU-flat skins and lightmapped/bump-fallback
        // TriMeshes do not enter either illumination path.
    }
    // K1 PartTriMesh::Draw 0x34ab08 gates the rigid shell by
    // AurBumpyShinyAvailable. PartSkin has its own VP/CPU admission instead.
    const bool drawShell=e.shellAmount!=0.0f &&
        (k2 || mesh.skinned || p.material.engineHardware.bumpyShiny);
    if(drawShell) {
        if(!textures.shell || !textures.shellMetadata || !textures.shellTxiAvailable) {
            error="Bumped-out shell texture or its engine TXI initialization is missing";return false;
        }
        if(textures.shellMetadata->facts.cubeMap ||
           ((textures.shellMetadata->facts.animated || textures.shellMetadata->txi.properties.procedureType) &&
            !submission.prepareTextures)) {
            error="A procedural/animated shell requires its engine draw-time texture binding adapter";return false;
        }
        if(mesh.skinned) {
            if(!next.deformed || !next.deformed->valid ||
               next.deformed->rawSkinNormals.size()!=mesh.vertices.size()) {
                error="Bumped-out skin shell requires the unnormalized weighted skin normals";return false;
            }
            for(const auto n:next.deformed->rawSkinNormals) if(!finite(n)) {
                error="Non-finite bumped-out skin normal";return false;
            }
        } else {
            // Deformed dangly normals are in the same space as their positions.
            if(next.deformed && next.deformed->valid) {
                if(next.deformed->normals.size()!=mesh.vertices.size()) {
                    error="Bumped-out mesh has an incomplete posed normal pool";return false;
                }
                for(std::size_t i=0;i<mesh.vertices.size();++i)
                    if(!finite(model::deformedNormalAt(mesh,*next.deformed,i))) {
                        error="Non-finite bumped-out posed normal";return false;
                    }
            } else for(std::size_t i=0;i<mesh.vertices.size();++i) if(!finite(mesh.normalAt(i))) {
                error="Non-finite bumped-out source normal";return false;
            }
        }
        auto shell=p;
        shell.ownerFragment=OwnerFragmentEffect::None;
        shell.skinEffect=SkinRenderEffect::BumpedOutShell;
        shell.shellAmount=e.shellAmount;
        const auto shellState=shellMaterial(textures.shellMetadata->txi);
        // DrawBumpedOut's gate omits usevertexprograms (0x10fcf4):
        // a CPU primary may still have a VP shell.
        shell.engineSkinVertexPrograms=p.engineSkinShellVertexPrograms;
        shell.engineCpuSkin=mesh.skinned && !shell.engineSkinVertexPrograms;
        shell.exactK2SkinVertexProgram=mesh.skinned && shell.engineSkinVertexPrograms;
        const bool cpuSkinShell=shell.engineCpuSkin;
        if(cpuSkinShell)shell.engineCpuSkinRestoreTexture=p.diffuseTexture;
        if(cpuSkinShell) {
            shell.material.pass=shellState.pass;
            shell.material.blendFunction=shellState.blendFunction;
            shell.material.blending=shellState.blending;
            shell.material.depthWrite=shellState.depthWrite;
            shell.material.alphaTest=shellState.alphaTest;
            shell.material.alphaCutoff=shellState.alphaCutoff;
            // Preserve authored/owner diffuse, self-illumination and alpha;
            // ExecuteSkinTransforms' CPU shell does not execute
            // the cBumpedOut white-color vertex program.
        } else shell.material=shellState;
        shell.material.engineGame=p.material.engineGame;
        shell.material.engineAtiValidation=p.material.engineAtiValidation;
        shell.diffuseTexture=textures.shell;
        shell.lightmapTexture=shell.bumpTexture=shell.environmentTexture=shell.hologramFilterTexture=0;
        if(!cpuSkinShell) {
            shell.lighting=false;shell.lightCount=0;
            shell.hasColorOverride=true;shell.colorOverride={{1,1,1,1}};
        }
        shell.basisDebug=0;shell.selected=false;shell.outlineOnly=false;
        next.trailingShell=std::move(shell);
    }
    if(!finite(p.colorOverride)||!finite(p.material.selfIllumination)||!std::isfinite(p.material.opacity)) {
        error="Owner material arithmetic overflowed the finite renderer domain";return false;
    }
    if(drawShell || p.ownerFragment!=OwnerFragmentEffect::None) {
        if(next.deformed && next.deformed->valid) {
            if(next.deformed->positions.size()!=mesh.vertices.size() ||
               next.deformed->normals.size()!=mesh.vertices.size()) {
                error="Owner effect has incomplete posed position/normal pools";return false;
            }
            for(const auto v:next.deformed->positions) if(!finite(v)) {
                error="Non-finite owner-effect position";return false;
            }
        } else for(const auto& v:mesh.vertices) if(!finite(v.position)) {
            error="Non-finite owner-effect source position";return false;
        }
    }
    submission=std::move(next);
    return true;
}
} // namespace neomdl::render
