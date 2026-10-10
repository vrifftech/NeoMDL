#include <neoshared/glu/ByteMipmaps.hpp>
#include "render/ShaderMeshRenderer.hpp"
#include "render/GrassPass.hpp"
#include "render/OwnerEffects.hpp"
#include "render/MeshShaders.hpp"
#include "render/MaterialPass.hpp"
#include "render/DesktopMaterialPrograms.hpp"
#include "render/EngineDesktopFragmentPrograms.hpp"
#include "render/DesktopFixedFunction.hpp"
#include "render/DirtyMaterialPass.hpp"
#include "render/SkinMaterialPass.hpp"
#include "render/K1MaterialPass.hpp"
#include <model/SceneOrdering.hpp>
#include <model/EngineCpuSkin.hpp>
#include <model/UvAnimation.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace neomdl::render {
namespace {
namespace g=gl;
using neomdl::model::RenderVertex;
static_assert(std::is_standard_layout<RenderVertex>::value, "GPU vertex must be standard layout");
static_assert(sizeof(RenderVertex)==112u, "Unexpected GPU vertex padding");

void enabled(g::Api& api, g::Enum cap, bool on) {
    if (on) api.Enable(cap); else api.Disable(cap);
}
struct AttributeState {
    g::Int enabled{}, size{}, type{}, normalized{}, stride{}, buffer{};
    void* pointer{};
    std::array<float,4> current{};
};
// Restore the actual incoming state, not guessed defaults. This includes
// attribute buffer associations, which remain attached after a VBO unbind.
struct StateGuard {
    g::Api& api;
    g::Int program{}, arrayBuffer{}, indexBuffer{}, active{}, textures[6]{}, cubes[6]{}, volumes[6]{};
    g::Int srcRgb{}, dstRgb{}, srcAlpha{}, dstAlpha{}, eqRgb{}, eqAlpha{};
    g::Int depthFunction{}, polygon[2]{}, cullMode{}, winding{};
    g::Boolean depthMask{}, blend{}, depth{}, alpha{}, cull{}, lighting{}, fog{}, textureEnabled[6]{};
    g::Int alphaFunction{};float alphaReference{};
    g::Boolean colorMaterial{};
    std::array<float,4> globalAmbient{},materialAmbient{},materialDiffuse{},materialEmission{};
    float lineWidth{};
    g::Boolean stencil{}, colorMask[4]{};
    g::Int stencilClear{}, stencilState[2][7]{};
    std::array<AttributeState,15> attributes{};
    g::Int attributeCount{}, textureCount{};
    explicit StateGuard(g::Api& a):api(a) {
        api.GetIntegerv(g::MaxVertexAttribs,&attributeCount);attributeCount=std::min(attributeCount,15);
        api.GetIntegerv(g::MaxTextureUnits,&textureCount);textureCount=std::min(textureCount,6);
        stencil=api.IsEnabled(g::StencilTest);
        api.GetBooleanv(g::ColorWriteMask,colorMask);
        api.GetIntegerv(g::StencilClearValue,&stencilClear);
        const g::Enum stencilKeys[2][7]={
            {g::StencilFunc,g::StencilRef,g::StencilValueMask,g::StencilWriteMask,g::StencilFail,g::StencilDepthFail,g::StencilDepthPass},
            {g::StencilBackFunc,g::StencilBackRef,g::StencilBackValueMask,g::StencilBackWriteMask,g::StencilBackFail,g::StencilBackDepthFail,g::StencilBackDepthPass}};
        for (unsigned face=0;face<2;++face) for (unsigned field=0;field<7;++field)
            api.GetIntegerv(stencilKeys[face][field],&stencilState[face][field]);
        api.GetIntegerv(g::CurrentProgram,&program);
        api.GetIntegerv(g::ArrayBufferBinding,&arrayBuffer);
        api.GetIntegerv(g::ElementArrayBufferBinding,&indexBuffer);
        api.GetIntegerv(g::ActiveTexture,&active);
        for (g::UInt i=0;i<static_cast<g::UInt>(textureCount);++i) {
            api.ActiveTexture(g::Texture0+i); api.GetIntegerv(g::TextureBinding2D,&textures[i]);
            textureEnabled[i]=api.IsEnabled(g::Texture2D);
            api.GetIntegerv(g::TextureBindingCube,&cubes[i]);
            api.GetIntegerv(0x806a,&volumes[i]);
        }
        api.ActiveTexture(static_cast<g::Enum>(active));
        api.GetIntegerv(g::BlendSrcRgb,&srcRgb); api.GetIntegerv(g::BlendDstRgb,&dstRgb);
        api.GetIntegerv(g::BlendSrcAlpha,&srcAlpha); api.GetIntegerv(g::BlendDstAlpha,&dstAlpha);
        api.GetIntegerv(g::BlendEquationRgb,&eqRgb); api.GetIntegerv(g::BlendEquationAlpha,&eqAlpha);
        api.GetIntegerv(g::DepthFunc,&depthFunction); api.GetBooleanv(g::DepthWriteMask,&depthMask);
        api.GetIntegerv(g::PolygonMode,polygon); api.GetFloatv(g::LineWidth,&lineWidth);
        api.GetIntegerv(g::CullFaceMode,&cullMode); api.GetIntegerv(g::FrontFace,&winding);
        blend=api.IsEnabled(g::Blend); depth=api.IsEnabled(g::DepthTest);
        alpha=api.IsEnabled(g::AlphaTest); cull=api.IsEnabled(g::CullFace);
        lighting=api.IsEnabled(g::Lighting);fog=api.IsEnabled(g::Fog);
        colorMaterial=api.IsEnabled(g::ColorMaterial);
        api.GetFloatv(g::LightModelAmbient,globalAmbient.data());
        api.GetMaterialfv(g::Front,g::MaterialAmbient,materialAmbient.data());
        api.GetMaterialfv(g::Front,g::MaterialDiffuse,materialDiffuse.data());
        api.GetMaterialfv(g::Front,g::MaterialEmission,materialEmission.data());
        api.GetIntegerv(g::AlphaTestFunc,&alphaFunction);api.GetFloatv(g::AlphaTestRef,&alphaReference);
        for (g::UInt i=0;i<static_cast<g::UInt>(attributeCount);++i) {
            auto& s=attributes[i];
            api.GetVertexAttribiv(i,g::AttribEnabled,&s.enabled);
            api.GetVertexAttribiv(i,g::AttribSize,&s.size);
            api.GetVertexAttribiv(i,g::AttribType,&s.type);
            api.GetVertexAttribiv(i,g::AttribNormalized,&s.normalized);
            api.GetVertexAttribiv(i,g::AttribStride,&s.stride);
            api.GetVertexAttribiv(i,g::AttribBufferBinding,&s.buffer);
            api.GetVertexAttribPointerv(i,g::AttribPointer,&s.pointer);
            if(i!=0)api.GetVertexAttribfv(i,g::CurrentVertexAttrib,s.current.data());
        }
    }
    ~StateGuard() {
        for (unsigned face=0;face<2;++face) {
            const auto side=face==0 ? g::Front : g::Back;
            const auto* values=stencilState[face];
            api.StencilFuncSeparate(side,static_cast<g::Enum>(values[0]),values[1],static_cast<g::UInt>(values[2]));
            api.StencilMaskSeparate(side,static_cast<g::UInt>(values[3]));
            api.StencilOpSeparate(side,static_cast<g::Enum>(values[4]),static_cast<g::Enum>(values[5]),static_cast<g::Enum>(values[6]));
        }
        api.ClearStencil(stencilClear);
        api.ColorMask(colorMask[0],colorMask[1],colorMask[2],colorMask[3]);
        enabled(api,g::StencilTest,stencil!=0);
        for (g::UInt i=0;i<static_cast<g::UInt>(attributeCount);++i) {
            const auto& s=attributes[i];
            api.BindBuffer(g::ArrayBuffer,static_cast<g::UInt>(s.buffer));
            api.VertexAttribPointer(i,s.size,static_cast<g::Enum>(s.type),
                static_cast<g::Boolean>(s.normalized),s.stride,s.pointer);
            if (s.enabled) api.EnableVertexAttribArray(i); else api.DisableVertexAttribArray(i);
            if(i!=0)api.VertexAttrib4fv(i,s.current.data());
        }
        api.BindBuffer(g::ArrayBuffer,static_cast<g::UInt>(arrayBuffer));
        api.BindBuffer(g::ElementArrayBuffer,static_cast<g::UInt>(indexBuffer));
        for (g::UInt i=0;i<static_cast<g::UInt>(textureCount);++i) {
            api.ActiveTexture(g::Texture0+i);
            api.BindTexture(g::Texture2D,static_cast<g::UInt>(textures[i]));
            api.BindTexture(g::TextureCube,static_cast<g::UInt>(cubes[i]));
            api.BindTexture(0x806f,static_cast<g::UInt>(volumes[i]));
            enabled(api,g::Texture2D,textureEnabled[i]!=0);
        }
        api.ActiveTexture(static_cast<g::Enum>(active));
        api.UseProgram(static_cast<g::UInt>(program));
        api.BlendFuncSeparate(static_cast<g::Enum>(srcRgb),static_cast<g::Enum>(dstRgb),
                              static_cast<g::Enum>(srcAlpha),static_cast<g::Enum>(dstAlpha));
        api.BlendEquationSeparate(static_cast<g::Enum>(eqRgb),static_cast<g::Enum>(eqAlpha));
        api.DepthMask(depthMask); api.DepthFunc(static_cast<g::Enum>(depthFunction));
        api.AlphaFunc(static_cast<g::Enum>(alphaFunction),alphaReference);
        enabled(api,g::Lighting,lighting!=0);enabled(api,g::Fog,fog!=0);
        api.Disable(g::ColorMaterial);
        api.LightModelfv(g::LightModelAmbient,globalAmbient.data());
        api.Materialfv(g::Front,g::MaterialAmbient,materialAmbient.data());
        api.Materialfv(g::Front,g::MaterialDiffuse,materialDiffuse.data());
        api.Materialfv(g::Front,g::MaterialEmission,materialEmission.data());
        enabled(api,g::ColorMaterial,colorMaterial!=0);
        api.PolygonMode(g::Front,static_cast<g::Enum>(polygon[0]));
        api.PolygonMode(g::Back,static_cast<g::Enum>(polygon[1]));
        api.LineWidth(lineWidth); api.CullFace(static_cast<g::Enum>(cullMode));
        api.FrontFace(static_cast<g::Enum>(winding));
        enabled(api,g::Blend,blend!=0); enabled(api,g::DepthTest,depth!=0);
        enabled(api,g::AlphaTest,alpha!=0); enabled(api,g::CullFace,cull!=0);
    }
};
std::string shaderLog(g::Api& api,g::UInt object,bool program) {
    g::Int length=0;
    if(program) api.GetProgramiv(object,g::InfoLogLength,&length);
    else api.GetShaderiv(object,g::InfoLogLength,&length);
    std::vector<char> data(static_cast<std::size_t>(std::max(length,1)), '\0');
    g::Size written=0;
    if(program) api.GetProgramInfoLog(object,static_cast<g::Size>(data.size()),&written,data.data());
    else api.GetShaderInfoLog(object,static_cast<g::Size>(data.size()),&written,data.data());
    return std::string(data.data(),static_cast<std::size_t>(std::clamp(written,0,static_cast<int>(data.size()))));
}
g::UInt compile(g::Api& api,g::Enum type,const char* source,std::string& error) {
    const auto shader=api.CreateShader(type);
    if(!shader) { error="OpenGL could not allocate a shader object"; return 0; }
    api.ShaderSource(shader,1,&source,nullptr); api.CompileShader(shader);
    g::Int ok=0; api.GetShaderiv(shader,g::CompileStatus,&ok);
    if(!ok) {
        error=std::string(type==g::VertexShader ? "Vertex" : "Fragment")+" shader failed: "+shaderLog(api,shader,false);
        api.DeleteShader(shader); return 0;
    }
    return shader;
}
}

struct ShaderMeshRenderer::Impl {
    g::Api api;
    bool gobNewSorting{true};
    material::ForceSightSettings forceSight;
    g::UInt program{},fixedProgram{};
    desktop_programs::LegacyApi fixedApi;
    SoftShadowPass softShadows;
    ScenePostprocess sceneEffects;
    GrassPass grass;
    DesktopStandardVertexState desktopStandard;
    bool distortionActive{};
    SkinMaterialPass skinMaterials;
    K1MaterialPass k1Materials;
    EngineSceneRasterState sceneState;
    bool sceneActive{};
    model::GameVersion sceneGame{model::GameVersion::Kotor2};
    g::UInt dialogueFadeProgram{},dialogueFadeVbo{},dialogueFadeIbo{};
    g::Int dialogueFadeColor{};
    EngineMaterialState materialState;
    desktop_programs::ProgramCache blurPrograms;
    g::UInt normalizationCube{};
    struct Uniforms {
        g::Int modelView{}, projection{}, normal{}, handedness{}, diffuseSampler{}, lightmapSampler{};
        g::Int useDiffuse{}, useLightmap{}, lighting{}, overrideColor{}, color{}, selfIllumination{}, basisDebug{}, lightCount{};
        g::Int lightPosition{}, lightDiffuse{}, lightAmbient{}, lightAttenuation{};
        g::Int meshAmbient{}, sceneAmbient{}, fogMode{}, fogColor{}, fogData{}, additive{};
        g::Int bumpSampler{}, envSampler{}, cubeSampler{}, bumpKind{}, envMode{}, eyeToWorld{};
        g::Int bumpScale{}, diffuseBump{}, specularBump{}, bumpIntensities{}, specularColor{};
        g::Int environmentStrength{}, alphaTest{}, alphaCutoff{}, textureAlpha{};
        g::Int diffuseUv{}, diffuseScroll{}, diffuseUvInfo{}, bumpUv{}, bumpScroll{}, bumpUvInfo{};
        g::Int skinEffect{}, exactK2Skin{}, skinMotion{}, skinMaximumBlurLength{};
        g::Int engineSkinBlur{},skinBlurDirection{},skinBlurSpeed{};
        g::Int skinBlurSideScale{}, skinCameraPosition{}, skinBlurSideDirection{};
        g::Int ownerFragment{}, holoSampler{}, worldFromEye{}, ownerIllumination{}, shellAmount{};
        g::Int materialProgram{}, partHasOwner{}, fixedNormalize{}, environmentAlpha{}, fixedTextureMatrix{}, engineFlat{};
        g::Int flareProgram{}, flareDesaturation{};
        g::Int dirtyCommand{},dirtyWorld{},dirtySampler{},dirtyForce{};
        g::Int forceCommand{},forceColor{},forceDesaturation{};
        g::Int fixedUv1Array{},fixedDirtyUnit{};
    } u,fixedU;

    gl::Loader fixedLoader;
    void uniformLocations(g::UInt program,Uniforms& u) {
        auto& a=api;
#define LOC(field,name) u.field=a.GetUniformLocation(program,name)
    LOC(modelView,"uModelView"); LOC(projection,"uProjection"); LOC(normal,"uNormalMatrix");
    LOC(handedness,"uTransformHandedness"); LOC(diffuseSampler,"uDiffuseTexture");
    LOC(lightmapSampler,"uLightmapTexture"); LOC(useDiffuse,"uUseDiffuse"); LOC(useLightmap,"uUseLightmap");
    LOC(lighting,"uLighting"); LOC(overrideColor,"uOverride"); LOC(color,"uColor");
    LOC(selfIllumination,"uSelfIllumination");
    LOC(basisDebug,"uBasisDebug"); LOC(lightCount,"uLightCount"); LOC(lightPosition,"uLightPosition[0]");
    LOC(meshAmbient,"uMeshAmbient"); LOC(sceneAmbient,"uSceneAmbient");
    LOC(fogMode,"uFogMode");LOC(fogColor,"uFogColor");LOC(fogData,"uFogData");LOC(additive,"uAdditive");
    LOC(lightDiffuse,"uLightDiffuse[0]"); LOC(lightAmbient,"uLightAmbient[0]"); LOC(lightAttenuation,"uLightAttenuation[0]");
    LOC(bumpSampler,"uBumpTexture"); LOC(envSampler,"uEnvironmentTexture"); LOC(cubeSampler,"uEnvironmentCube");
    LOC(bumpKind,"uBumpKind"); LOC(envMode,"uEnvironmentMode"); LOC(eyeToWorld,"uEyeToWorld");
    LOC(bumpScale,"uBumpScale"); LOC(diffuseBump,"uDiffuseBump"); LOC(specularBump,"uSpecularBump");
    LOC(bumpIntensities,"uBumpIntensities"); LOC(specularColor,"uSpecularColor");
    LOC(environmentStrength,"uEnvironmentStrength"); LOC(alphaTest,"uAlphaTest");
    LOC(alphaCutoff,"uAlphaCutoff"); LOC(textureAlpha,"uTextureAlphaIsOpacity");
    LOC(diffuseUv,"uDiffuseUv"); LOC(diffuseScroll,"uDiffuseScroll"); LOC(diffuseUvInfo,"uDiffuseUvInfo");
    LOC(bumpUv,"uBumpUv"); LOC(bumpScroll,"uBumpScroll"); LOC(bumpUvInfo,"uBumpUvInfo");
    LOC(skinEffect,"uSkinEffect"); LOC(exactK2Skin,"uExactK2Skin");
    LOC(skinMotion,"uSkinMotion"); LOC(skinMaximumBlurLength,"uSkinMaximumBlurLength");
    LOC(engineSkinBlur,"uEngineSkinBlur");LOC(skinBlurDirection,"uSkinBlurDirection");LOC(skinBlurSpeed,"uSkinBlurSpeed");
    LOC(skinBlurSideScale,"uSkinBlurSideScale");
    LOC(skinCameraPosition,"uSkinCameraPosition");
    LOC(skinBlurSideDirection,"uSkinBlurSideDirection");
    LOC(ownerFragment,"uOwnerFragment"); LOC(holoSampler,"uHologramFilter");
    LOC(worldFromEye,"uWorldFromEye"); LOC(ownerIllumination,"uOwnerIllumination");
    LOC(shellAmount,"uShellAmount");
    LOC(forceCommand,"uForceSightCommand");LOC(forceColor,"uForceSightColor");LOC(forceDesaturation,"uForceSightDesaturation");
    LOC(dirtyCommand,"uDirtyCommand");LOC(dirtyWorld,"uDirtyWorld");LOC(dirtySampler,"uDirtyTexture");LOC(dirtyForce,"uDirtyForce");
    LOC(flareProgram,"uFlareProgram"); LOC(flareDesaturation,"uFlareDesaturation");
    LOC(engineFlat,"uEngineFlat");
    LOC(materialProgram,"uMaterialProgram"); LOC(partHasOwner,"uPartHasOwner");
    LOC(fixedTextureMatrix,"uFixedTextureMatrix");
    LOC(fixedNormalize,"uFixedNormalize"); LOC(environmentAlpha,"uEnvironmentAlpha");
    LOC(fixedUv1Array,"uFixedUv1Array");LOC(fixedDirtyUnit,"uFixedDirtyUnit");
#undef LOC
    }
    bool prepareFixed(std::string& error) {
        if(fixedProgram)return true;
        if(!fixedApi.load(fixedLoader,error))return false;
        auto& a=api;
        // Engine no-ARB dispatch retains the actual compatibility fragment
        // stage; only its vertex arithmetic is translated by this program.
        std::string source=meshVertexShader;
        source.insert(source.find('\n')+1,std::string("#define ENGINE_FIXED_FRAGMENT\n")+desktopFixedCoordinatesGlsl);
        const auto vertex=compile(a,g::VertexShader,source.c_str(),error);
        if(!vertex)return false;
        const auto program=a.CreateProgram();a.AttachShader(program,vertex);
        const char* names[]={"aPosition","aNormal","aUv0","aUv1","aTangent",
                             "aBitangent","aBasisNormal","aColor","aRawNormal"};
        for(g::UInt n=0;n<9;++n)a.BindAttribLocation(program,n,names[n]);
        a.LinkProgram(program);a.DeleteShader(vertex);
        g::Int linked{};a.GetProgramiv(program,g::LinkStatus,&linked);
        if(!linked){error="Engine fixed-fragment vertex link failed: "+shaderLog(a,program,true);a.DeleteProgram(program);return false;}
        fixedProgram=program;uniformLocations(fixedProgram,fixedU);return true;
    }
    struct CachedMesh {
        g::UInt vbo{}, ebo{}, skinVbo{}, rigidVbo{};
        std::shared_ptr<const model::EngineCpuSkinInput> cpuSourceInput;
        std::shared_ptr<const model::EngineCpuSkinSource> builtCpuSourceInput;
        std::optional<model::EngineCpuSkinSource> cpuSource;
        std::uint64_t cpuNormalRevision{};
        std::shared_ptr<const StaticPrelightPool::Identity> prelight;
        std::uint64_t prelightGeneration{};
        std::shared_ptr<const StaticPrelightPool::Identity> rigidPrelight;
        std::uint64_t rigidPrelightGeneration{};
        g::Size count{};
        std::size_t bytes{};
        const void* vertices{};
        const void* indices{};
        std::size_t vertexCount{}, indexCount{};
        std::uint64_t revision{};
        std::uint64_t normalRevision{},skinNormalRevision{},rigidNormalRevision{};
        const model::ProceduralUvPool* proceduralUv{};
        std::uint64_t proceduralUvRevision{};
        const model::DeformedMesh* pose{};
        bool posed{}, streamed{};
    };
    std::unordered_map<const neomdl::model::Mesh*,CachedMesh> meshes;
    std::vector<g::UInt> retiredBuffers;
    model::EngineCpuSkinPools cpuSkinPools;
    RendererStatistics stats;
    neomdl::model::Mesh shadowStream;
    std::uint64_t shadowRevision{};
    model::Mesh flareStream;
    std::uint64_t flareRevision{};
};
ShaderMeshRenderer::ShaderMeshRenderer():impl_(std::make_unique<Impl>()) {}
// Context owners call shutdown while current. If their surface has already
// disappeared, context destruction releases GPU objects; never issue GL calls
// from a destructor with an unknown current context.
ShaderMeshRenderer::~ShaderMeshRenderer()=default;
bool ShaderMeshRenderer::ready() const noexcept { return impl_->program!=0; }
RendererStatistics ShaderMeshRenderer::statistics() const noexcept {
    auto result=impl_->stats; result.cachedMeshes=impl_->meshes.size(); return result;
}
bool ShaderMeshRenderer::initialize(const g::Loader& resolver,std::string& error) {
    error.clear(); if(ready()) return true;
    auto& i=*impl_; auto& a=i.api;
    if(!a.load(resolver,error)) return false;
    const auto* version=a.GetString(g::Version);
    int major=0,minor=0;
    if(!version || std::sscanf(reinterpret_cast<const char*>(version),"%d.%d",&major,&minor)!=2 ||
        major<2 || (major==2 && minor<1)) {
        error="NeoMDL requires a desktop OpenGL 2.1+ compatibility context with GLSL 1.20";
        return false;
    }
    if(major>3 || (major==3 && minor>=2)) {
        g::Int profile=0;
        a.GetIntegerv(0x9126u,&profile); // GL_CONTEXT_PROFILE_MASK
        if((profile & 1)!=0) {
            error="NeoMDL currently requires a compatibility profile; helper geometry has not migrated to core OpenGL";
            return false;
        }
    }
    g::Int attributes=0,units=0;
    a.GetIntegerv(g::MaxVertexAttribs,&attributes); a.GetIntegerv(g::MaxTextureUnits,&units);
    if(attributes<9 || units<6) { error="OpenGL needs nine vertex attributes and six fragment texture units"; return false; }
    const auto vertex=compile(a,g::VertexShader,meshVertexShader,error);
    if(!vertex) return false;
    const auto fragment=compile(a,g::FragmentShader,meshFragmentShader,error);
    if(!fragment) { a.DeleteShader(vertex); return false; }
    const auto program=a.CreateProgram();
    if(!program) { a.DeleteShader(vertex); a.DeleteShader(fragment); error="OpenGL could not allocate a shader program"; return false; }
    a.AttachShader(program,vertex); a.AttachShader(program,fragment);
    const char* names[]={"aPosition","aNormal","aUv0","aUv1","aTangent",
                         "aBitangent","aBasisNormal","aColor","aRawNormal"};
    for(g::UInt n=0;n<9;++n) a.BindAttribLocation(program,n,names[n]);
    a.LinkProgram(program); a.DeleteShader(vertex); a.DeleteShader(fragment);
    g::Int linked=0; a.GetProgramiv(program,g::LinkStatus,&linked);
    if(!linked) { error="Mesh shader link failed: "+shaderLog(a,program,true); a.DeleteProgram(program); return false; }
    i.program=program;
    i.uniformLocations(i.program,i.u);
    i.fixedLoader=resolver;
    // Engine Scene construction eagerly acquires the normalization cube. Its
    // GL_NORMALIZE side effect belongs to logical engine state, never host GL.
    try {
        StateGuard guard(a);
        g::Int maximum=0;a.GetIntegerv(0x851c,&maximum);
        if(maximum<256)throw std::runtime_error("Engine normalization cube requires 256-square cube faces");
        g::Int unpackBuffer=0;const g::Enum keys[]={0x0cf5,0x0cf2,0x0cf3,0x0cf4};
        g::Int values[4];a.GetIntegerv(0x88ef,&unpackBuffer);
        for(unsigned k=0;k<4;++k)a.GetIntegerv(keys[k],&values[k]);
        struct UnpackRestore {g::Api& a;g::Int buffer;const g::Enum* keys;g::Int* values;
            ~UnpackRestore(){a.BindBuffer(0x88ec,buffer);for(unsigned k=0;k<4;++k)a.PixelStorei(keys[k],values[k]);}
        } unpack{a,unpackBuffer,keys,values};
        a.BindBuffer(0x88ec,0);a.PixelStorei(keys[0],1);
        for(unsigned k=1;k<4;++k)a.PixelStorei(keys[k],0);
        a.GenTextures(1,&i.normalizationCube);
        if(!i.normalizationCube)throw std::runtime_error("Cannot allocate engine normalization cube");
        a.ActiveTexture(g::Texture1);a.BindTexture(g::TextureCube,i.normalizationCube);
        g::Int gluMaximum=0;a.GetIntegerv(0x0d33,&gluMaximum);
        for(unsigned face=0;face<6;++face) {
            const auto levels=neoshared::glu::buildByteMipmaps(
                {256,256,3,normalizationCubeFace(face)},unsigned(gluMaximum));
            for(std::size_t level=0;level<levels.size();++level) {
                const auto& image=levels[level];
                a.TexImage2D(0x8515+face,g::Int(level),0x1907,image.width,image.height,
                    0,0x1907,0x1401,image.bytes.data());
            }
        }
        a.TexParameteri(g::TextureCube,0x2800,0x2601);
        a.TexParameteri(g::TextureCube,0x2801,0x2601);
        a.TexParameteri(g::TextureCube,0x2802,0x812f);
        a.TexParameteri(g::TextureCube,0x2803,0x812f);
        if(a.GetError()!=g::NoError)throw std::runtime_error("Engine normalization-cube upload failed");
        i.materialState.reset();i.materialState.initializeNormalization(i.normalizationCube);
        g::Int textureStackDepth{};a.GetIntegerv(0x0d39,&textureStackDepth);
        i.materialState.setTextureMatrixStackLimit(static_cast<unsigned>(textureStackDepth));
        i.blurPrograms.loader=resolver;
        i.skinMaterials.setLoader(resolver);
        i.grass.setLoader(resolver);
        i.k1Materials.setLoader(resolver);
    } catch(const std::exception& e) {
        if(i.normalizationCube)a.DeleteTextures(1,&i.normalizationCube);
        i.normalizationCube=0;a.DeleteProgram(i.program);i.program=0;error=e.what();return false;
    }
    i.softShadows.setResolver(resolver);
    i.sceneEffects.setResolver(resolver);
    return true;
}

bool ShaderMeshRenderer::consumeEnginePartMaterial(const model::Mesh& mesh,
        const MeshDrawParameters& parameters,std::string& error) {
    error.clear();auto p=parameters;auto& i=*impl_;
    const bool k1=p.material.engineGame==material::EngineMaterialGame::Kotor1;
    auto& cache=i.materialState.partMaterials[k1?0:1];
    if(i.sceneActive) {
        cache.image.ambient=i.sceneState.materialAmbient;cache.image.diffuse=i.sceneState.materialDiffuse;
        cache.image.emission=i.sceneState.materialEmission;i.sceneState.lighting=p.lighting;
    }
    cache.image.vertex={{i.desktopStandard.emission,i.desktopStandard.diffuse,i.desktopStandard.ambient}};
    // Mat's owner branch precedes ignoremat and the material upload cache.
    // Its virtual texture Bind remains observable even though Draw returns.
    if(p.partHasOwner) {
        if(p.rebindEngineTexture) {
            p.diffuseTexture=p.rebindEngineTexture(p.diffuseTexture,error);
            if(!error.empty())return false;
        }
        const auto unit=i.materialState.fixedActiveTextureUnit;
        i.materialState.bind(unit,false,p.diffuseTexture);
        if(p.material.engineHardware.cubeMap)i.materialState.fixedTextureUnits[unit].enabled[1]=false;
        i.materialState.fixedTextureUnits[unit].enabled[2]=false;
        i.materialState.alphaTestEnabled=true;i.materialState.alphaReference=p.material.alphaCutoff;
        if(i.sceneActive) {
            i.sceneState.alphaTest=true;
            i.sceneState.alphaReference=p.material.alphaCutoff;i.sceneState.depthWrite=p.material.depthWrite;
            const auto blend=p.material.engineMaterialBlendFunction;
            i.sceneState.source=blend==material::BlendFunction::Replace?g::One:g::SrcAlpha;
            i.sceneState.destination=blend==material::BlendFunction::Replace?g::Zero:
                blend==material::BlendFunction::Additive?g::One:g::OneMinusSrcAlpha;
        }
    }
    prepareEnginePartMaterial(mesh,p,cache);
    i.desktopStandard.emission=cache.image.vertex[0];i.desktopStandard.diffuse=cache.image.vertex[1];
    i.desktopStandard.ambient=cache.image.vertex[2];i.desktopStandard.fixedEmission=cache.image.emission;
    if(i.sceneActive) {
        i.sceneState.materialAmbient=cache.image.ambient;i.sceneState.materialDiffuse=cache.image.diffuse;
        i.sceneState.materialEmission=cache.image.emission;
    }
    return true;
}

bool ShaderMeshRenderer::draw(const neomdl::model::Mesh& mesh,
    const neomdl::model::DeformedMesh* deformed,std::uint64_t revision,
    const MeshDrawParameters& parameters,std::string& error) {
    MeshDrawParameters p=parameters;
    const bool k1DesktopScene=impl_->sceneActive &&
        impl_->sceneGame==model::GameVersion::Kotor1;
    if(k1DesktopScene && (p.particlePass || p.lightsaberPass ||
       p.engineFlareProgram!=EngineFlareProgram::None)) {
        // K1 particle helpers toggle alpha testing but never AlphaFunc.
        // Saber Draw inherits enable; its preceding Mat supplies the cutoff.
        if(!p.engineLightsaberMaterial)
            p.material.alphaCutoff=impl_->sceneState.alphaReference;
        if(!p.particlePass)p.material.alphaTest=impl_->sceneState.alphaTest;
        if(p.lightsaberPass) {
            p.material.depthWrite=p.oneOneBlend?false:p.engineLightsaberMaterial
                ?p.material.depthWrite:impl_->sceneState.depthWrite;
            p.cullBackfaces=false;
        }
    }
    p.engineCpuSkin=p.material.k2PartTriMeshState && mesh.skinned && !p.engineSkinVertexPrograms;
    std::optional<model::RenderGeometry> cpuGeometry;
    model::DeformedMesh cpuPose;
    const model::EngineCpuSkinSource* cpuSource=nullptr;
    if(p.engineCpuSkin) {
        // The non-VP caller ignores the blur argument and never enters the
        // skin hologram interception. Environment illumination remains live.
        if(p.skinEffect==SkinRenderEffect::MotionBlur)p.skinEffect=SkinRenderEffect::None;
        if(p.ownerFragment==OwnerFragmentEffect::Hologram)p.ownerFragment=OwnerFragmentEffect::None;
        p.exactK2SkinVertexProgram=false;
        try {
            if(!deformed)throw std::invalid_argument("Engine skin material requires its current pose");
            auto& cache=impl_->meshes[&mesh];
            if(cache.cpuSourceInput!=mesh.engineCpuSkin ||
               cache.builtCpuSourceInput!=mesh.builtCpuSkinSource ||
               cache.cpuNormalRevision!=mesh.normalRevision()) {
                cache.cpuSource=model::decodeEngineCpuSkinSource(mesh);
                cache.cpuSourceInput=mesh.engineCpuSkin;
                cache.builtCpuSourceInput=mesh.builtCpuSkinSource;
                cache.cpuNormalRevision=mesh.normalRevision();
            }
            cpuSource=&*cache.cpuSource;
            cpuGeometry=impl_->cpuSkinPools.execute(mesh,*deformed,*cpuSource,
                p.skinEffect==SkinRenderEffect::BumpedOutShell,p.shellAmount);
            p.modelView=model::multiply(p.modelView,deformed->skinPartFrame);
            cpuPose.valid=true;
            for(const auto& vertex:cpuGeometry->vertices) {
                cpuPose.positions.push_back(vertex.position);
                cpuPose.normals.push_back(vertex.rawNormal);
            }
            deformed=&cpuPose;
        } catch(const std::exception& e) {error=e.what();return false;}
    }
    const bool engineK1Draw=!mesh.animated && p.material.k2PartTriMeshState &&
        p.material.engineGame==material::EngineMaterialGame::Kotor1;
    const auto& drawIndices=p.engineCpuSkin?cpuSource->indices:
        engineK1Draw?mesh.engineDrawIndices:mesh.indices;
    const auto& drawBatches=p.engineCpuSkin?cpuSource->draws:mesh.engineDraws;
    const bool cpuBumpFallback=p.engineCpuSkin && p.material.engineSkinBumpTangent &&
        (!p.material.engineHardware.nv1xLevelShaders || p.material.engineAtiValidation) &&
        !p.material.skinEnvironmentAvailable && p.skinEffect==SkinRenderEffect::None;

    if(impl_->distortionActive) {
        p.selected=false;p.outlineOnly=false;
    }
    auto nextMaterialState=impl_->materialState;
    error.clear(); if(!ready()) { error="Mesh shader backend is not initialized"; return false; }
    if(mesh.vertices.empty() || drawIndices.empty()) {
        if((p.material.k2PartTriMeshState||p.engineLightsaberMaterial)&&!p.inspectMaterial&&!p.outlineOnly&&
           !p.particlePass&&p.engineFlareProgram==EngineFlareProgram::None&&p.engineShadowPass==EngineShadowPass::None)
            return consumeEnginePartMaterial(mesh,p,error);
        return true;
    }
    if(mesh.animated) {
        // Animmesh draws consume the current pool and engine batches,
        // including the primitive4 direct-index tail, without texture rotation.
        // Validate the complete batch sequence before any pass, texture event,
        // or framebuffer output (also protects manually constructed meshes).
        for (const auto& batch:mesh.engineDraws) {
            if (batch.primitive>=10 || batch.firstIndex>mesh.indices.size() ||
                batch.indexCount>mesh.indices.size()-batch.firstIndex ||
                batch.indexCount>static_cast<std::size_t>(std::numeric_limits<g::Size>::max())) {
                error="Invalid animmesh engine draw batch";return false;
            }
        }
        if(!deformed || !deformed->valid || !deformed->animmesh ||
           !deformed->animmesh->positionsDefined ||
           (!deformed->animmesh->texcoordsDefined && static_cast<std::int32_t>(mesh.mdxTexcoordOffsets[0])>=0) ||
           deformed->animmesh->localPositions.size()!=mesh.vertices.size() ||
           deformed->animmesh->texcoords.size()!=mesh.vertices.size()) {
            error="Engine animmesh has an undefined/missing current position or UV pool";return false;
        }
        // Callers pass the model-space deformation frame (no second node
        // transform). Engine rendering keeps the local pool and part matrix so
        // the engine inverse-transpose normal transform is preserved.
        p.modelView=model::multiply(p.modelView,deformed->animMeshPartFrame);
        p.engineAnimMesh=true;
        if(p.skinEffect==SkinRenderEffect::None) {
            p.ownerFragment=OwnerFragmentEffect::None;
            p.material.renderPath=material::TriMeshRenderPath::Flat;
            p.material.useLightmap=false;p.material.useEnvironmentMap=false;p.material.useBumpMap=false;
        }
    }
    if(p.engineBlurTraversal) {
        g::Boolean depth{};impl_->api.GetBooleanv(g::DepthWriteMask,&depth);
        p.material.depthWrite=depth!=0;p.material.blending=true;
    }
    const bool forceSight=p.forceSight.enabled && p.material.engineGame==material::EngineMaterialGame::Kotor2;
    // RenderSinglePass enables blending for static geometry; DoGobBuckets
    // enables it before each Gob draw (Desktop0xc2b76). Mat only changes factors.
    if(forceSight && p.material.k2PartTriMeshState && !p.particlePass && !p.lightsaberPass)
        p.material.blending=true;
    if(forceSight && !mesh.skinned && p.material.renderPath==material::TriMeshRenderPath::BumpmapFallback) {
        // Its Force Sight branch precedes the non-Force alpha disable and
        // ONE/ZERO override (Desktop0xd5451).
        p.material.alphaTest=true;p.material.blendFunction=p.material.engineMaterialBlendFunction;
    }
    if(forceSight && p.dirty.settings.enabled) {
        p.dirty.settings.forceSight=true;p.dirty.settings.desaturation=p.forceSight.desaturation;
        p.dirty.settings.forceAlpha=material::setupForceSight(p.forceSight,
            p.partHasOwner?&p.forceSightOwner:nullptr,0,0,false,false).color[3];
    }
    MaterialProgram engineProgram;
    if(!resolveMaterialProgram(p,engineProgram,error)) return false;
    const bool enginePartMat=(p.material.k2PartTriMeshState||p.engineLightsaberMaterial)&&
        !p.inspectMaterial&&!p.outlineOnly&&!p.particlePass&&
        p.engineFlareProgram==EngineFlareProgram::None&&p.engineShadowPass==EngineShadowPass::None;
    if(enginePartMat) {
        auto& cache=nextMaterialState.partMaterials[p.material.engineGame==material::EngineMaterialGame::Kotor1?0:1];
        if(impl_->sceneActive) {
            cache.image.ambient=impl_->sceneState.materialAmbient;
            cache.image.diffuse=impl_->sceneState.materialDiffuse;
            cache.image.emission=impl_->sceneState.materialEmission;
        }
        cache.image.vertex={{impl_->desktopStandard.emission,impl_->desktopStandard.diffuse,impl_->desktopStandard.ambient}};
        prepareEnginePartMaterial(mesh,p,cache);
    }
    try { nextMaterialState.prepare(p,engineProgram); }
    catch(const std::exception& e){error=e.what();return false;}
    if(k1DesktopScene && p.engineLightsaberMaterial) {
        nextMaterialState.bind(0,false,p.diffuseTexture);
        p.fixedTextureMatrix=nextMaterialState.textureMatrix(0);
    }
    const bool dirty=engineProgram==MaterialProgram::Dirty;
    if(dirty&&(mesh.skinned||mesh.animated||p.particlePass||p.lightsaberPass)) {
        error="RenderNoiseMapped is a rigid PartTriMesh caller, not a skin/animmesh/effect substitute";return false;
    }
    material::DirtyMaterialPlan dirtyPlan;
    if(dirty && !material::makeDirtyMaterialPlan(p.dirty,p.material.useLightmap,p.material.useEnvironmentMap,dirtyPlan,error,!p.material.engineHardware.arbFragmentPrograms))return false;
    const bool engineMaterial=engineProgram!=MaterialProgram::Preview;
    const bool engineVertex=engineMaterial && engineProgram!=MaterialProgram::FlatFixed;
    material::ForceSightPlan forcePlan;
    if(dirty&&dirtyPlan.fixedDispatch)
        forcePlan=material::setupForceSight(p.forceSight,p.partHasOwner?&p.forceSightOwner:nullptr,
            dirtyPlan.command,dirtyPlan.command,false,false);
    int forceCommand=0;
    const bool desktopForce=forceSight && !p.inspectMaterial && !p.outlineOnly && !p.engineAnimMesh && !p.lightsaberPass &&
        p.engineFlareProgram==EngineFlareProgram::None && p.engineShadowPass==EngineShadowPass::None &&
        p.skinEffect!=SkinRenderEffect::MotionBlur &&
        (!p.exactK2SkinVertexProgram || p.ownerFragment==OwnerFragmentEffect::Hologram ||
         p.skinEffect==SkinRenderEffect::BumpedOutShell) && !dirty &&
        (!p.engineCpuSkin || p.material.skinEnvironmentAvailable);
    const bool desktopFixedForce=desktopForce && !p.material.engineHardware.arbFragmentPrograms;
    const bool desktopEngineBlur=p.engineSkinBlur&&p.skinEffect==SkinRenderEffect::MotionBlur;
    const bool desktopFixedFragment=desktopFixedForce || p.engineDesktopFixedFragment || (dirty&&dirtyPlan.fixedDispatch);
    if(desktopForce) {
        if(p.particlePass) {forcePlan.command=56;forcePlan.color={{0,0,0,1}};}
        else if(p.ownerFragment==OwnerFragmentEffect::Hologram)forcePlan.command=76;
        else {
            const bool env=engineProgram==MaterialProgram::Environment ||
                engineProgram==MaterialProgram::EnvironmentLightmap ||
                p.ownerFragment==OwnerFragmentEffect::IlluminationEnvironment;
            const bool lm=engineProgram==MaterialProgram::Lightmap || engineProgram==MaterialProgram::EnvironmentLightmap;
            forcePlan=material::setupForceSight(p.forceSight,p.partHasOwner?&p.forceSightOwner:nullptr,
                env?(lm?53u:52u):(lm?51u:50u),env?(lm?59u:58u):(lm?57u:56u),
                false,p.forceSightIllumination);
        }
        if(!desktopFixedForce)switch(forcePlan.command) {
        case 50:case 52:case 56:case 57:case 58:case 59:case 92:case 93:
            nextMaterialState.forceSightFragmentCommand=forcePlan.command;
            nextMaterialState.fragmentConstants[1]=forcePlan.illumination
                ?std::array<float,4>{{p.ownerIllumination[0],p.ownerIllumination[1],p.ownerIllumination[2],1}}:forcePlan.color;
            break;
        case 76:nextMaterialState.forceSightFragmentCommand=76;break;
        default:break; // Desktop dispatcher has empty cases; preserve prior FP
        }
        if(!desktopFixedForce)if(const auto* fragment=desktop_programs::k2FragmentProgram(forcePlan.command)) {
            nextMaterialState.bindFragmentProgram(fragment);
            nextMaterialState.fragmentConstants[7]={{.3f,.59f,.11f,forcePlan.command==76?.75f:
                forcePlan.command==50||forcePlan.command==52?.8f:p.forceSight.desaturation}};
        }
        forceCommand=nextMaterialState.forceSightFragmentCommand
            ?static_cast<int>(nextMaterialState.forceSightFragmentCommand):-1;
    }
    // Ordinary Desktop helpers are independent producers of the retained FP.
    // Flat/shell/animmesh calls can leave it unchanged; cleanup commands do
    // not all unbind it. The ARB blur caller later consumes this exact image.
    int ordinaryFragmentCleanup=-1;
    if(p.material.engineGame==material::EngineMaterialGame::Kotor2 &&p.material.engineHardware.arbFragmentPrograms&&
       p.material.k2PartTriMeshState&&!p.inspectMaterial&&!p.outlineOnly&&!desktopForce&&!dirty&&
       !p.particlePass&&!p.lightsaberPass&&p.engineFlareProgram==EngineFlareProgram::None&&
       p.engineShadowPass==EngineShadowPass::None&&!p.engineAnimMesh&&!p.engineSkinBlur) {
        unsigned command=0;bool cube=false;
        if(p.ownerFragment==OwnerFragmentEffect::Hologram) {command=75;ordinaryFragmentCleanup=77;}
        else if(p.ownerFragment==OwnerFragmentEffect::Illumination) {
            if(p.material.engineHardware.nv1xLevelShaders)command=78;
            ordinaryFragmentCleanup=0;
        } else if(p.ownerFragment==OwnerFragmentEffect::IlluminationEnvironment) {command=79;ordinaryFragmentCleanup=32;}
        else if(engineProgram==MaterialProgram::Lightmap) {command=26;ordinaryFragmentCleanup=32;}
        else if(engineProgram==MaterialProgram::Environment) {command=19;cube=p.environmentCube;ordinaryFragmentCleanup=32;}
        else if(engineProgram==MaterialProgram::EnvironmentLightmap) {command=22;cube=p.lightmapCube;ordinaryFragmentCleanup=37;}
        if(command)if(const auto* fragment=desktop_programs::k2FragmentProgram(command,cube)) {
            nextMaterialState.bindFragmentProgram(fragment);
            if(command==75)nextMaterialState.fragmentConstants[7]={{.3f,.59f,.11f,.75f}};
            if(command==78||command==79)nextMaterialState.fragmentConstants[1]={{
                p.ownerIllumination[0],p.ownerIllumination[1],p.ownerIllumination[2],1}};
        }
    }
    auto& i=*impl_; auto& a=i.api;
    const auto priorError=a.GetError();
    if(priorError!=g::NoError) {
        error="OpenGL already had a pending error before mesh submission ("+std::to_string(priorError)+")";
        return false;
    }
    // PartSkin::Mat precedes the engine zero-displacement guard. Keep its
    // texture/alpha state, but do not run the VP, command34, or command0.
    if(p.skinEffect==SkinRenderEffect::MotionBlur &&
       (p.engineSkinBlur?!p.skinBlurMotionAdmitted:(p.skinMotion.x==0 && p.skinMotion.y==0 && p.skinMotion.z==0))) {
        if(k1DesktopScene) {
            i.sceneState.alphaTest=p.material.alphaTest;
            i.sceneState.alphaReference=p.material.alphaCutoff;
        }
        i.materialState=nextMaterialState;
        return true;
    }
    if(desktopFixedFragment && !i.prepareFixed(error))return false;
    StateGuard restore(a);
    if(enginePartMat&&p.enginePartMaterial) {
        i.desktopStandard.emission=p.enginePartMaterial->vertex[0];
        i.desktopStandard.diffuse=p.enginePartMaterial->vertex[1];
        i.desktopStandard.ambient=p.enginePartMaterial->vertex[2];
        i.desktopStandard.fixedEmission=p.enginePartMaterial->emission;
    }
    if(enginePartMat&&impl_->sceneActive) {
        const auto& image=*p.enginePartMaterial;
        impl_->sceneState.materialAmbient=image.ambient;
        impl_->sceneState.materialDiffuse=image.diffuse;
        impl_->sceneState.materialEmission=image.emission;
    }
    std::unique_ptr<desktop_programs::LegacyGuard> fixedRestore;
    if(desktopFixedFragment)fixedRestore=std::make_unique<desktop_programs::LegacyGuard>(a,i.fixedApi);
    auto& u=desktopFixedFragment?i.fixedU:i.u;
    struct EngineTextureMatrixScope {
        EngineMaterialState& current;EngineMaterialState& target;bool active;
        ~EngineTextureMatrixScope() {
            if(active) {
                // RestoreTextureMatrix0xcc172 uses the EXIT active unit, not
                // the entry unit. Preserve engine stack errors and history.
                current.popTextureMatrix(current.fixedActiveTextureUnit);target=current;
            }
        }
    } textureMatrixScope{nextMaterialState,i.materialState,
        forceSight && !p.material.engineHardware.arbFragmentPrograms && p.material.k2PartTriMeshState &&
        !p.engineAnimMesh && !p.inspectMaterial && !p.outlineOnly && p.textureAngleDegrees!=0.0f};
    if(textureMatrixScope.active)
        nextMaterialState.pushTextureMatrix(nextMaterialState.fixedActiveTextureUnit,engineTextureRotation(p.textureAngleDegrees));
    // Cleanup belongs to the engine call even when a virtual Bind throws.
    // Host GL restoration remains the outer guard's separate responsibility.
    struct DirtyCleanup {
        EngineMaterialState& target;EngineMaterialState& current;unsigned command;bool fixed{},active{};
        ~DirtyCleanup(){if(active){if(!fixed)current.finishDirty(command);else current.fixedCurrentStage=0;target=current;}}
    } dirtyCleanup{i.materialState,nextMaterialState,dirtyPlan.cleanupCommand,!p.material.engineHardware.arbFragmentPrograms};
    if(dirty) {
        dirtyCleanup.active=true;
        if(!prepareDirtyBindings(a,p,dirtyPlan,nextMaterialState,error))return false;
    }
    const bool engineSkin=engineProgram==MaterialProgram::SkinBump || engineProgram==MaterialProgram::SkinBumpyShiny ||
            engineProgram==MaterialProgram::SkinFlat || engineProgram==MaterialProgram::SkinEnvironment;
    const bool k1Rigid=engineProgram==MaterialProgram::K1Rigid;
    const bool generatedColorArray=!engineSkin&&!p.engineCpuSkin&&!p.engineOnlyGob&&p.lighting;
    // Flat::SetupArrays binds generated colors first, then SetInterleavedBuffer
    // replaces that pointer when the animmesh MDX bitmap contains authored
    // colors (K2 Desktop 0xb2cf4/0xb2d4b, K1 ARM 0x5c9950/0x5c9990).
    // The color-array enable state is independent of this pointer selection.
    const bool authoredAnimMeshColors=mesh.animated && (mesh.mdxBitmap&0x40u)!=0;
    if(p.enginePrelightPool&&p.enginePrelightPool->active&&generatedColorArray&&!authoredAnimMeshColors)
        for(const auto index:drawIndices)if(index>=p.enginePrelightPool->colors.size()) {
            // LightPartTriMesh allocates count+1 but does not write the extra
            // word. An actual reference to it needs captured engine memory;
            // authored MDX color/white cannot stand in for an undefined pool.
            error="Engine generated-color draw references an unwritten prelighting pool value";
            return false;
        }
    if(k1Rigid && !i.k1Materials.prepare(a,p,error))return false;
    if(engineSkin) {
        try {
            if(!deformed)throw std::invalid_argument("Engine skin material requires its current pose");
            validateEngineSkinFrame(mesh,*deformed,engineProgram==MaterialProgram::SkinBump || engineProgram==MaterialProgram::SkinBumpyShiny);
        } catch(const std::exception& e) {error=e.what();return false;}
        if(!i.skinMaterials.prepare(a,p,engineProgram,error))return false;
    }
    const bool posed=deformed!=nullptr;
    try {
        auto& cache=i.meshes[&mesh];
        const bool newGeometry=!cache.vbo || cache.vertices!=mesh.vertices.data() ||
            cache.indices!=drawIndices.data() || cache.vertexCount!=mesh.vertices.size() ||
            cache.indexCount!=drawIndices.size();
        const auto normalRevision=mesh.normalRevision();
        const bool normalsChanged=cache.normalRevision!=normalRevision;
        const auto* prelight=p.enginePrelightPool;
        const auto prelightIdentity=prelight?prelight->identity:nullptr;
        const auto prelightGeneration=prelight?prelight->generation:0;
        const bool prelightChanged=cache.prelight!=prelightIdentity || cache.prelightGeneration!=prelightGeneration;
        // VertexPrimitiveFlat::SetupArrays (K2 Desktop 0xb2d09) binds the
        // model-owned MDX pool. PartAnimMesh then replaces UV0 with its own
        // controller pool (0xb2d57); procedural writes do not replace that.
        const auto* proceduralUv=mesh.animated ? nullptr : mesh.proceduralUvPool.get();
        const auto uvRevision=proceduralUv ? proceduralUv->revision : 0u;
        const bool uvChanged=cache.proceduralUv!=proceduralUv || cache.proceduralUvRevision!=uvRevision;
        const auto applyUv=[&](auto& vertices, auto member) {
            if (proceduralUv)
                for (std::size_t vertex=0;vertex<vertices.size();++vertex)
                    vertices[vertex].*member=proceduralUv->texcoords[vertex];
        };
        const auto applyPrelight=[&](auto& vertices) {
            if(prelight && prelight->active && !authoredAnimMeshColors)
                for(std::size_t vertex=0;vertex<prelight->colors.size();++vertex)
                    for(unsigned channel=0;channel<4;++channel)
                        vertices[vertex].color[channel]=static_cast<float>(prelight->colors[vertex][channel])/255.0f;
        };
        const bool refresh=p.engineCpuSkin || newGeometry || normalsChanged || prelightChanged || uvChanged || cache.posed!=posed || cache.streamed!=p.streamedGeometry ||
            ((posed || p.streamedGeometry) && (cache.revision!=revision || cache.pose!=deformed));
        if(refresh) {
            auto geometry=p.engineCpuSkin?std::move(*cpuGeometry):
                neomdl::model::prepareRenderGeometry(mesh,posed ? deformed : nullptr,true);
            applyPrelight(geometry.vertices);
            applyUv(geometry.vertices,&RenderVertex::texcoord0);
            if(engineK1Draw)geometry.indices=drawIndices;
            if(p.primitive!=PrimitiveTopology::Triangles) {
                // A lightsaber stream is already the exact
                // GL_QUAD_STRIP index order from PartLightsaber::Draw,
                // including repeated transition vertices and an odd endpoint.
                geometry.indices.clear();
                geometry.indices.reserve(mesh.indices.size());
                for(const auto index:mesh.indices) {
                    if(index>=mesh.vertices.size()) {
                        error="Stream primitive index exceeds the vertex buffer";
                        return false;
                    }
                    geometry.indices.push_back(index);
                }
            }
            if(geometry.indices.size()>static_cast<std::size_t>(std::numeric_limits<g::Size>::max()) ||
                geometry.vertices.size()>static_cast<std::size_t>(std::numeric_limits<g::Bytes>::max())/sizeof(RenderVertex) ||
                geometry.indices.size()>static_cast<std::size_t>(std::numeric_limits<g::Bytes>::max())/sizeof(std::uint32_t)) {
                error="Mesh exceeds OpenGL buffer/count limits"; return false;
            }
            const auto bytes=geometry.vertices.size()*sizeof(RenderVertex);
            if(!cache.vbo) a.GenBuffers(1,&cache.vbo);
            if(!cache.ebo) a.GenBuffers(1,&cache.ebo);
            if(!cache.vbo || !cache.ebo) { error="OpenGL could not allocate mesh buffers"; return false; }
            a.BindBuffer(g::ArrayBuffer,cache.vbo);
            if(bytes!=cache.bytes || newGeometry)
                a.BufferData(g::ArrayBuffer,static_cast<g::Bytes>(bytes),geometry.vertices.data(),(posed || p.streamedGeometry) ? g::DynamicDraw : g::StaticDraw);
            else a.BufferSubData(g::ArrayBuffer,0,static_cast<g::Bytes>(bytes),geometry.vertices.data());
            a.BindBuffer(g::ElementArrayBuffer,cache.ebo);
            // CPU deformation cannot change topology; the IBO is uploaded only
            // at allocation/replacement, not on every animation frame.
            if(newGeometry || p.streamedGeometry || cache.count!=static_cast<g::Size>(geometry.indices.size()))
                a.BufferData(g::ElementArrayBuffer,static_cast<g::Bytes>(geometry.indices.size()*sizeof(std::uint32_t)),
                             geometry.indices.data(),p.streamedGeometry ? g::DynamicDraw : g::StaticDraw);
            const auto uploadError=a.GetError();
            if(uploadError!=g::NoError) {
                // A failed allocation/update must not be marked cached; retry
                // creates fresh storage instead of drawing stale/unallocated data.
                if(cache.vbo) a.DeleteBuffers(1,&cache.vbo);
                if(cache.ebo) a.DeleteBuffers(1,&cache.ebo);
                if(cache.skinVbo) a.DeleteBuffers(1,&cache.skinVbo);
                if(cache.rigidVbo) a.DeleteBuffers(1,&cache.rigidVbo);
                cache={};
                error="OpenGL mesh buffer upload failed ("+std::to_string(uploadError)+")";
                return false;
            }
            cache.count=static_cast<g::Size>(geometry.indices.size()); cache.bytes=bytes;
            cache.vertices=mesh.vertices.data(); cache.indices=drawIndices.data();
            cache.vertexCount=mesh.vertices.size(); cache.indexCount=drawIndices.size();
            cache.revision=revision; cache.pose=deformed; cache.posed=posed; cache.streamed=p.streamedGeometry;
            cache.normalRevision=normalRevision;
            cache.prelight=prelightIdentity;cache.prelightGeneration=prelightGeneration;
            cache.proceduralUv=proceduralUv;cache.proceduralUvRevision=uvRevision;
            if(posed || p.streamedGeometry) ++i.stats.dynamicUploads; else ++i.stats.staticUploads;
        }
        if(cache.count==0) return true;
        const auto indexedDraw=[&](g::Enum fallback) {
            if (!mesh.animated && !p.engineCpuSkin && !engineK1Draw &&
                !(p.lightsaberPass && !mesh.engineDraws.empty())) {
                a.DrawElements(fallback,cache.count,g::UnsignedInt,nullptr);++i.stats.drawCalls;return;
            }
            static constexpr g::Enum modes[]={0,1,3,4,5,6,7,8,9,2};
            for (const auto& batch:drawBatches) {
                a.DrawElements(modes[batch.primitive],static_cast<g::Size>(batch.indexCount),g::UnsignedInt,
                    reinterpret_cast<const void*>(batch.firstIndex*sizeof(std::uint32_t)));
                ++i.stats.drawCalls;
            }
        };
        bool enginePrimary=false;
        if(k1Rigid && !p.outlineOnly) {
            if(refresh || !cache.rigidVbo || cache.rigidNormalRevision!=normalRevision || cache.rigidPrelight!=prelightIdentity ||
               cache.rigidPrelightGeneration!=prelightGeneration) {
                auto vertices=engineRigidVertices(mesh,posed?deformed:nullptr);
                applyPrelight(vertices);
                applyUv(vertices,&EngineRigidVertex::uv0);
                if(!cache.rigidVbo)a.GenBuffers(1,&cache.rigidVbo);
                a.BindBuffer(g::ArrayBuffer,cache.rigidVbo);
                a.BufferData(g::ArrayBuffer,static_cast<g::Bytes>(vertices.size()*sizeof(EngineRigidVertex)),
                    vertices.data(),posed||p.streamedGeometry?g::DynamicDraw:g::StaticDraw);
                cache.rigidPrelight=prelightIdentity;cache.rigidPrelightGeneration=prelightGeneration;
                cache.rigidNormalRevision=normalRevision;
            }
            a.BindBuffer(g::ElementArrayBuffer,cache.ebo);
            if(!i.k1Materials.draw(a,mesh,p,nextMaterialState,i.normalizationCube,
                cache.rigidVbo,cache.count,drawBatches,i.stats.drawCalls,error))return false;
            enginePrimary=true;
            if(!p.selected) {
                if(k1DesktopScene) {
                    i.sceneState.alphaTest=nextMaterialState.alphaTestEnabled;
                    i.sceneState.alphaReference=nextMaterialState.alphaReference;
                }
                i.materialState=nextMaterialState;
                return true;
            }
        }
        if(engineSkin && !p.outlineOnly) {
            if(!posed){error="Engine skin material requires its current bone palette";return false;}
            if(!cache.skinVbo || newGeometry || cache.skinNormalRevision!=normalRevision || uvChanged || p.streamedGeometry) {
                auto vertices=engineSkinVertices(mesh,*deformed, engineProgram==MaterialProgram::SkinBump || engineProgram==MaterialProgram::SkinBumpyShiny);
                applyUv(vertices,&EngineSkinVertex::uv);
                if(vertices.size()>static_cast<std::size_t>(std::numeric_limits<g::Bytes>::max())/sizeof(EngineSkinVertex)) {
                    error="Engine skin vertex buffer exceeds OpenGL limits";return false;
                }
                if(!cache.skinVbo)a.GenBuffers(1,&cache.skinVbo);
                if(!cache.skinVbo){error="Cannot allocate engine skin source buffer";return false;}
                a.BindBuffer(g::ArrayBuffer,cache.skinVbo);
                a.BufferData(g::ArrayBuffer,static_cast<g::Bytes>(vertices.size()*sizeof(EngineSkinVertex)),vertices.data(),g::StaticDraw);
                if(a.GetError()!=g::NoError){a.DeleteBuffers(1,&cache.skinVbo);cache.skinVbo=0;error="Engine skin source upload failed";return false;}
                cache.skinNormalRevision=normalRevision;
                ++i.stats.engineSkinUploads;
            }
            a.BindBuffer(g::ElementArrayBuffer,cache.ebo);
            if(!i.skinMaterials.draw(a,mesh,*deformed,p,engineProgram,nextMaterialState,i.normalizationCube,cache.skinVbo,cache.count,drawBatches,i.stats.drawCalls,error))return false;
            enginePrimary=true;
        }
        a.BindBuffer(g::ArrayBuffer,cache.vbo); a.BindBuffer(g::ElementArrayBuffer,cache.ebo);
        const g::Int sizes[]={3,3,2,2,4,3,4,4,3};
        const std::size_t offsets[]={offsetof(RenderVertex,position),offsetof(RenderVertex,normal),
            offsetof(RenderVertex,texcoord0),offsetof(RenderVertex,texcoord1),
            offsetof(RenderVertex,tangent),offsetof(RenderVertex,bitangent),
            offsetof(RenderVertex,basisNormal),
            offsetof(RenderVertex,color),offsetof(RenderVertex,rawNormal)};
        for(g::UInt slot=0;slot<9;++slot) {
            a.EnableVertexAttribArray(slot);
            a.VertexAttribPointer(slot,sizes[slot],g::Float,0,sizeof(RenderVertex),
                                  reinterpret_cast<const void*>(offsets[slot]));
        }
        if(p.engineAnimMesh && engineProgram==MaterialProgram::FlatFixed &&
           static_cast<std::int32_t>(mesh.mdxNormalOffset)<0) {
            // SetInterleavedBuffer leaves a missing NORMAL_ARRAY disabled.
            // The engine's current ordinary normal starts at (0,0,1).
            const float currentNormal[]={0,0,1,1};
            a.DisableVertexAttribArray(8);a.VertexAttrib4fv(8,currentNormal);
        }
        if((p.engineAnimMesh || p.engineCpuSkin) && engineProgram==MaterialProgram::FlatFixed &&
           static_cast<std::int32_t>(mesh.mdxTexcoordOffsets[0])<0) {
            // RenderFlat +0x160 tests UV0, NOT the normal offset +0x158.
            // Consume the explicit engine current coordinate when the array is
            // disabled. Host GL attribute values are unrelated and restored.
            a.DisableVertexAttribArray(2);a.VertexAttrib4fv(2,p.engineFlatCurrentTexcoord.data());
        }
        if(desktopForce && !desktopFixedForce && !p.particlePass) {
            const auto bindForce=[&](unsigned unit,bool cube,gl::UInt& texture) {
                if(p.rebindEngineTexture) {
                    texture=p.rebindEngineTexture(texture,error);
                    if(!texture)throw std::runtime_error(error);
                }
                nextMaterialState.bind(unit,cube,texture);
                auto& target=nextMaterialState.fixedTextureUnits[unit];
                if(p.material.engineHardware.cubeMap)target.enabled[1]=cube;
                target.enabled[2]=false;
            };
            const unsigned command=forcePlan.command;
            if(command==52 || command==58 || command==57 || command==59) {
                bindForce(0,false,p.diffuseTexture);
                if(command==52||command==58)bindForce(1,p.environmentCube,p.environmentTexture);
                if(command==59)bindForce(2,p.environmentCube,p.environmentTexture);
                if(command==57||command==59) {
                    nextMaterialState.fixedTextureUnits[0].enabled[0]=true;
                    nextMaterialState.fixedTextureUnits[1].enabled[0]=true;
                    nextMaterialState.fixedSecondTexture=true;
                    bindForce(1,p.lightmapCube,p.lightmapTexture);
                }
                if(command==52||command==58||command==59)nextMaterialState.fixedEnvironmentMap=nextMaterialState.fixedSecondTexture=true;
            }
            if(command==52||command==58||command==59) {
                p.environmentTexture=nextMaterialState.binding(command==59?2:1,false);
                p.environmentCube=false; // original fragments sample the retained 2D target
            }
        }
        if(desktopFixedForce) {
            if(p.particlePass)nextMaterialState.bind(0,false,p.diffuseTexture);
            if(!applyDesktopFixedFragmentCommand(forcePlan.command,p,nextMaterialState,error))return false;
        }
        a.UseProgram(desktopFixedFragment?i.fixedProgram:i.program);
        const auto normal=neomdl::model::makeNormalMatrix(p.modelView);
        const float handedness=neomdl::model::transformHandedness(p.modelView);
        a.UniformMatrix4fv(u.modelView,1,0,p.modelView.values.data());
        a.UniformMatrix4fv(u.projection,1,0,p.projection.values.data());
        a.UniformMatrix3fv(u.normal,1,0,normal.data());
        a.Uniform1f(u.handedness,handedness);
        a.Uniform1i(u.skinEffect,p.engineCpuSkin?0:static_cast<g::Int>(p.skinEffect));
        a.Uniform1i(u.ownerFragment,static_cast<g::Int>(p.ownerFragment));
        const auto vertexMaterial=dirty?(dirtyPlan.environment?
            (dirtyPlan.fixedDispatch&&!dirtyPlan.lightmap?MaterialProgram::Environment:MaterialProgram::EnvironmentLightmap):
            dirtyPlan.lightmap?MaterialProgram::Lightmap:MaterialProgram::FlatVertex):engineProgram;
        a.Uniform1i(u.materialProgram,static_cast<g::Int>(vertexMaterial));
        a.Uniform1i(u.forceCommand,desktopFixedForce?0:forceCommand);
        a.Uniform4fv(u.forceColor,1,nextMaterialState.fragmentConstants[1].data());
        a.Uniform1f(u.forceDesaturation,p.forceSight.desaturation);
        a.Uniform1i(u.dirtyCommand,dirty?static_cast<int>(dirtyPlan.command):0);
        a.Uniform1i(u.dirtyWorld,p.dirty.settings.worldSpace==1);
        a.Uniform1i(u.dirtySampler,5);
        const float dirtyForce[]={p.dirty.settings.desaturation,p.dirty.settings.forceAlpha,0};
        a.Uniform3fv(u.dirtyForce,1,dirtyForce);
        a.ActiveTexture(g::Texture0+5);a.BindTexture(0x806f,dirty?p.dirty.texture:0u);
        a.Uniform1i(u.engineFlat,p.engineAnimMesh || p.engineCpuSkin || p.engineLightsaberMaterial);
        a.Uniform1i(u.flareProgram,static_cast<g::Int>(p.engineFlareProgram));
        a.Uniform1f(u.flareDesaturation,p.flareDesaturation);
        a.Uniform1i(u.partHasOwner,p.partHasOwner);
        a.Uniform1i(u.fixedNormalize,p.fixedFunctionNormalizeNormals.value_or(i.materialState.normalizeNormals()));
        a.UniformMatrix4fv(u.fixedTextureMatrix,1,0,p.fixedTextureMatrix.values.data());
        a.UniformMatrix4fv(u.worldFromEye,1,0,p.worldFromEye.values.data());
        a.Uniform3fv(u.ownerIllumination,1,p.ownerIllumination.data());
        a.Uniform1f(u.shellAmount,p.shellAmount);
        a.Uniform1i(u.holoSampler,1);
        a.Uniform1i(u.exactK2Skin,p.exactK2SkinVertexProgram ? 1 : 0);
        const float skinMotion[]={p.skinMotion.x,p.skinMotion.y,p.skinMotion.z};
        const float skinCamera[]={p.skinCameraPosition.x,p.skinCameraPosition.y,
                                  p.skinCameraPosition.z};
        const float skinSide[]={p.skinBlurSideDirection.x,p.skinBlurSideDirection.y,
                                p.skinBlurSideDirection.z};
        a.Uniform3fv(u.skinMotion,1,skinMotion);
        a.Uniform1i(u.engineSkinBlur,p.engineSkinBlur);
        const float blurDirection[]={p.skinBlurDirection.x,p.skinBlurDirection.y,p.skinBlurDirection.z};
        a.Uniform3fv(u.skinBlurDirection,1,blurDirection);a.Uniform1f(u.skinBlurSpeed,p.skinBlurSpeed);
        a.Uniform1f(u.skinMaximumBlurLength,p.skinMaximumBlurLength);
        a.Uniform1f(u.skinBlurSideScale,p.skinBlurSideScale);
        a.Uniform3fv(u.skinCameraPosition,1,skinCamera);
        a.Uniform3fv(u.skinBlurSideDirection,1,skinSide);
        // Engine program selection is authoritative. A stale material flag must
        // not leak a previous path's texture stages into this draw.
        const bool useLightmap=engineMaterial ?
            (engineProgram==MaterialProgram::Lightmap ||
             engineProgram==MaterialProgram::EnvironmentLightmap || (dirty&&dirtyPlan.lightmap)) : p.material.useLightmap;
        const bool useEnvironment=engineMaterial ?
            (engineProgram==MaterialProgram::Environment ||
             engineProgram==MaterialProgram::EnvironmentLightmap || (dirty&&dirtyPlan.environment)) : p.material.useEnvironmentMap;
        const bool useBump=!engineMaterial && p.material.useBumpMap;
        if(p.engineCpuSkin && p.skinEffect==SkinRenderEffect::BumpedOutShell) {
            // ExecuteSkinTransforms performs the shell's virtual Bind here,
            // then restores Material::BindTexture0 after the flat draw.
            if(p.rebindEngineTexture) {
                p.diffuseTexture=p.rebindEngineTexture(p.diffuseTexture,error);
                if(!p.diffuseTexture)return false;
            }
            nextMaterialState.bind(0,false,p.diffuseTexture);
        }
        a.ActiveTexture(g::Texture0); a.BindTexture(g::Texture2D,p.diffuseTexture);
        a.ActiveTexture(g::Texture1); a.BindTexture(g::Texture2D,
            p.ownerFragment==OwnerFragmentEffect::Hologram ? p.hologramFilterTexture :
            (useLightmap ? p.lightmapTexture : 0u));
        a.Uniform1i(u.diffuseSampler,0); a.Uniform1i(u.lightmapSampler,1);
        a.Uniform1i(u.useDiffuse,p.diffuseTexture!=0);
        a.Uniform1i(u.useLightmap,useLightmap && p.lightmapTexture!=0);
        a.Uniform1i(u.fogMode,static_cast<int>(p.fog.mode));
        const float fogColor[]={p.fog.color.x,p.fog.color.y,p.fog.color.z};
        const float fogData[]={p.fog.start,p.fog.end,p.fog.density};
        a.Uniform3fv(u.fogColor,1,fogColor);a.Uniform3fv(u.fogData,1,fogData);
        a.Uniform1i(u.additive,
            p.material.blendFunction==neomdl::material::BlendFunction::Additive);
        a.Uniform1i(u.lighting,p.lighting); a.Uniform1i(u.overrideColor,0);
        a.Uniform1i(u.basisDebug,std::clamp(p.basisDebug,0,3));
        const auto& material=p.material;
        a.ActiveTexture(g::Texture0+2); a.BindTexture(g::Texture2D,
            (useBump || (dirty&&!dirtyPlan.samplesVolume)) ? p.bumpTexture : 0u);
        a.ActiveTexture(g::Texture0+3); a.BindTexture(g::Texture2D,
            useEnvironment && !p.environmentCube ? p.environmentTexture : 0u);
        a.ActiveTexture(g::Texture0+4); a.BindTexture(g::TextureCube,
            useEnvironment && p.environmentCube ? p.environmentTexture : 0u);
        a.Uniform1i(u.bumpSampler,2); a.Uniform1i(u.envSampler,3); a.Uniform1i(u.cubeSampler,4);
        a.Uniform1i(u.bumpKind,useBump && p.bumpTexture
            ? static_cast<int>(material.bumpKind) : 0);
        a.Uniform1i(u.envMode,useEnvironment && p.environmentTexture
            ? (p.environmentCube ? 2 : 1) : 0);
        a.UniformMatrix3fv(u.eyeToWorld,1,0,p.eyeToWorld.data());
        a.Uniform1f(u.bumpScale,material.bumpScale);
        a.Uniform1i(u.diffuseBump,material.diffuseBump);
        a.Uniform1i(u.specularBump,material.specularBump);
        const float bumpIntensities[]={material.diffuseBumpIntensity,material.specularBumpIntensity,0};
        a.Uniform3fv(u.bumpIntensities,1,bumpIntensities);
        a.Uniform3fv(u.specularColor,1,material.specularColor.data());
        a.Uniform1f(u.environmentStrength,material.environmentStrength);
        const bool engineNoAlphaTest=(!desktopForce && engineProgram==MaterialProgram::FlatFixed &&
            ((!p.engineAnimMesh && !p.engineCpuSkin && !p.engineLightsaberMaterial) || cpuBumpFallback)) ||
            engineProgram==MaterialProgram::Environment ||
            engineProgram==MaterialProgram::EnvironmentLightmap;
        a.Uniform1i(u.alphaTest,!engineNoAlphaTest && (material.alphaTest ||
            (!material.k2PartTriMeshState && !p.engineLightsaberMaterial &&
             material.pass==neomdl::material::RenderPass::PunchThrough)));
        a.Uniform1f(u.alphaCutoff,material.alphaCutoff);
        a.Uniform1i(u.textureAlpha,material.textureAlphaIsOpacity);
        const auto setUv=[&](const neomdl::material::UvState& uv,g::Int transform,g::Int scroll,g::Int info) {
            const float values[]={uv.scale[0],uv.scale[1],uv.offset[0],uv.offset[1]};
            const float movement[]={uv.scroll[0],uv.scroll[1],uv.atlas ? 1.0f : 0.0f};
            const float sampling[]={uv.texel[0],uv.texel[1],uv.clamp ? 1.0f : 0.0f};
            a.Uniform4fv(transform,1,values); a.Uniform3fv(scroll,1,movement); a.Uniform3fv(info,1,sampling);
        };
        setUv(material.diffuseUv,u.diffuseUv,u.diffuseScroll,u.diffuseUvInfo);
        setUv(material.bumpUv,u.bumpUv,u.bumpScroll,u.bumpUvInfo);
        std::array<float,4> color{{
            p.hasColorOverride ? p.colorOverride[0] : mesh.diffuse.x,
            p.hasColorOverride ? p.colorOverride[1] : mesh.diffuse.y,
            p.hasColorOverride ? p.colorOverride[2] : mesh.diffuse.z,
            engineMaterial && !p.partHasOwner ? material.environmentOpacity :
            p.hasColorOverride ? (p.colorOverride[3] *
                (p.engineLightsaberMaterial ? 1.0f :
                 engineProgram==MaterialProgram::FlatFixed ? material.instanceOpacity*material.fixedFunctionAlphaScale :
                 engineVertex ? material.instanceOpacity : 1.0f))
                : (engineVertex ? material.vertexOpacity : material.opacity)}};
        if(p.enginePartMaterial)color=engineVertex?p.enginePartMaterial->vertex[1]:p.enginePartMaterial->diffuse;
        a.Uniform4fv(u.color,1,color.data());
        a.Uniform1f(u.environmentAlpha,p.hasColorOverride ? p.colorOverride[3] : material.environmentOpacity);
        a.Uniform3fv(u.selfIllumination,1,p.enginePartMaterial
            ?(engineVertex?p.enginePartMaterial->vertex[0].data():p.enginePartMaterial->emission.data())
            :material.selfIllumination.data());
        std::array<float,32> positions{};
        const bool engineIllum=engineMaterial || p.ownerFragment==OwnerFragmentEffect::Illumination ||
            p.ownerFragment==OwnerFragmentEffect::IlluminationEnvironment;
        std::array<float,3> meshAmbient{{engineIllum ? mesh.ambient.x : std::max(0.0f,mesh.ambient.x),
            engineIllum ? mesh.ambient.y : std::max(0.0f,mesh.ambient.y),
            engineIllum ? mesh.ambient.z : std::max(0.0f,mesh.ambient.z)}};
        if(p.enginePartMaterial)std::copy_n((engineVertex?p.enginePartMaterial->vertex[2]:p.enginePartMaterial->ambient).begin(),3,meshAmbient.begin());
        const std::array<float,3> noAmbient{};
        a.Uniform3fv(u.meshAmbient,1,meshAmbient.data());
        a.Uniform3fv(u.sceneAmbient,1,p.enginePartMaterial&&engineVertex?noAmbient.data():p.sceneAmbient.data());
        std::array<float,24> diffuse{},ambient{},attenuation{};
        const auto count=std::min(p.lightCount,p.lights.size());
        for(std::size_t l=0;l<count;++l) {
            std::copy(p.lights[l].position.begin(),p.lights[l].position.end(),positions.begin()+static_cast<std::ptrdiff_t>(4*l));
            std::copy(p.lights[l].diffuse.begin(),p.lights[l].diffuse.end(),diffuse.begin()+static_cast<std::ptrdiff_t>(3*l));
            std::copy(p.lights[l].ambient.begin(),p.lights[l].ambient.end(),ambient.begin()+static_cast<std::ptrdiff_t>(3*l));
            std::copy(p.lights[l].attenuation.begin(),p.lights[l].attenuation.end(),attenuation.begin()+static_cast<std::ptrdiff_t>(3*l));
        }
        a.Uniform1i(u.lightCount,static_cast<g::Int>(count));
        a.Uniform4fv(u.lightPosition,8,positions.data());
        a.Uniform3fv(u.lightDiffuse,8,diffuse.data()); a.Uniform3fv(u.lightAmbient,8,ambient.data());
        a.Uniform3fv(u.lightAttenuation,8,attenuation.data());
        enabled(a,g::CullFace,p.cullBackfaces); a.CullFace(g::Back);
        a.FrontFace(p.engineSkinBlur&&p.skinEffect==SkinRenderEffect::MotionBlur
            ?(p.skinBlurClockwise?g::CW:g::CCW):(handedness<0?g::CW:g::CCW));
        if(p.outlineOnly)a.Disable(g::StencilTest); // inspection must not replace engine scene stencil
        enabled(a,g::DepthTest,p.engineFlareProgram==EngineFlareProgram::None); if(!dirty&&!p.engineBlurTraversal)a.DepthFunc(g::Lequal);
        const bool lightsaberAdditive = p.lightsaberPass && p.oneOneBlend;
        const bool skinBlur=p.skinEffect==SkinRenderEffect::MotionBlur;
        const bool depthWrite = (skinBlur||p.engineFlareProgram!=EngineFlareProgram::None) ? false :
            p.lightsaberPass ? (k1DesktopScene?p.material.depthWrite:!lightsaberAdditive) :
            p.particlePass ? false : material.depthWrite;
        a.DepthMask(depthWrite ? 1 : 0);
        enabled(a,g::Blend,skinBlur || p.lightsaberPass || p.engineFlareProgram!=EngineFlareProgram::None || material.blending);
        a.Disable(g::AlphaTest);
        a.BlendEquationSeparate(g::FuncAdd,g::FuncAdd);
        // RenderMotionBlur establishes ordinary alpha blending around the
        // insertion-ordered blur bucket.
        if(p.engineFlareProgram!=EngineFlareProgram::None) {
            a.BlendFuncSeparate(g::One,g::One,g::One,g::One);
        } else if(skinBlur) {
            a.BlendFuncSeparate(g::SrcAlpha,g::OneMinusSrcAlpha,
                g::SrcAlpha,g::OneMinusSrcAlpha);
        // PartLightsaber uses ordinary glBlendFunc factors for both RGB and
        // alpha. Its optional blur-add branch is GL_ONE/GL_ONE.
        } else if(p.ownerFragment==OwnerFragmentEffect::Hologram) {
            const auto source=forceSight?g::SrcAlpha:g::One;
            const auto destination=forceSight?g::One:g::SrcAlpha;
            a.BlendFuncSeparate(source,destination,source,destination);
        } else if((desktopForce || (dirty&&dirtyPlan.fixedDispatch)) && forcePlan.replaceBlend) {
            const auto source=forcePlan.oneSource?g::One:g::SrcAlpha;
            a.BlendFuncSeparate(source,g::OneMinusSrcAlpha,source,g::OneMinusSrcAlpha);
        } else if(lightsaberAdditive) {
            a.BlendFuncSeparate(g::One,g::One,g::One,g::One);
        } else if(p.lightsaberPass) {
            const bool engineMat=k1DesktopScene && p.engineLightsaberMaterial;
            const auto source=engineMat
                ?(material.blendFunction==material::BlendFunction::Replace?g::One:g::SrcAlpha)
                :k1DesktopScene?i.sceneState.source:g::SrcAlpha;
            const auto destination=engineMat
                ?(material.blendFunction==material::BlendFunction::Replace?g::Zero:
                  material.blendFunction==material::BlendFunction::Additive?g::One:g::OneMinusSrcAlpha)
                :k1DesktopScene?i.sceneState.destination:g::OneMinusSrcAlpha;
            a.BlendFuncSeparate(source,destination,source,destination);
        } else if((!desktopForce && engineProgram==MaterialProgram::FlatFixed &&
            ((!p.engineAnimMesh && !p.engineCpuSkin) || cpuBumpFallback)) || (!p.particlePass &&
                  material.blendFunction==neomdl::material::BlendFunction::Replace)) {
            a.BlendFuncSeparate(g::One,g::Zero,g::One,g::Zero);
        } else {
            const bool additive=material.blendFunction==
                neomdl::material::BlendFunction::Additive;
            // K2 Material::SetBlendingMode uses glBlendFunc, so its source and
            // destination factors apply to both RGB and alpha. Retain the older
            // viewer alpha-channel policy only for non-K2 ordinary meshes.
            const bool ordinaryGlBlendFunc=material.k2PartTriMeshState;
            const auto sourceAlpha=(p.particlePass || ordinaryGlBlendFunc)
                ? g::SrcAlpha : g::One;
            const auto destinationAlpha=additive &&
                (p.particlePass || ordinaryGlBlendFunc)
                    ? g::One : g::OneMinusSrcAlpha;
            a.BlendFuncSeparate(g::SrcAlpha,
                additive ? g::One : g::OneMinusSrcAlpha,
                sourceAlpha,destinationAlpha);
        }
        if(k1DesktopScene && p.lightsaberPass && lightsaberAdditive)
            a.StencilMaskSeparate(g::FrontAndBack,0);
        if (p.engineShadowPass!=EngineShadowPass::None) {
            a.Enable(g::StencilTest);a.DepthMask(0);
            if (p.engineShadowPass==EngineShadowPass::Fade) {
                a.ColorMask(1,1,1,1);a.Disable(g::DepthTest);a.Disable(g::CullFace);
                a.Enable(g::Blend);
                if (p.shadowSoftMask)
                    a.BlendFuncSeparate(g::One,g::Zero,g::One,g::Zero);
                else
                    a.BlendFuncSeparate(g::SrcAlpha,g::OneMinusSrcAlpha,g::SrcAlpha,g::OneMinusSrcAlpha);
                a.StencilMaskSeparate(g::FrontAndBack,0u);
                a.StencilFuncSeparate(g::FrontAndBack,g::Lequal,static_cast<g::Int>(p.shadowStencilThreshold),0xffffffffu);
                a.StencilOpSeparate(g::FrontAndBack,g::Keep,g::Keep,g::Keep);
            } else if (p.engineShadowPass==EngineShadowPass::TwoSidedDepthPass) {
                // Desktop GLRender::RenderShadowArray 0x96884..0x96937. The engine
                // EXT branch updates on DEPTH PASS with wrapping arithmetic,
                // unlike the ordinary two-draw depth-fail branch below.
                a.ColorMask(0,0,0,0);a.Disable(g::Blend);a.Disable(g::CullFace);
                a.FrontFace(g::CCW);
                a.StencilMaskSeparate(g::FrontAndBack,0xffffffffu);
                a.StencilFuncSeparate(g::FrontAndBack,g::Always,0,0xffffffffu);
                a.StencilOpSeparate(g::Back,g::Keep,g::Keep,g::DecrWrap);
                a.StencilOpSeparate(g::Front,g::Keep,g::Keep,g::IncrWrap);
            } else {
                a.ColorMask(0,0,0,0);a.Disable(g::Blend);a.Enable(g::CullFace);
                a.CullFace(g::Back);
                const bool increment=p.engineShadowPass==EngineShadowPass::Increment;
                a.FrontFace(increment ? g::CW : g::CCW);
                a.StencilMaskSeparate(g::FrontAndBack,p.shadowStencilOff ? 0u : 0xffu);
                a.StencilFuncSeparate(g::FrontAndBack,g::Always,15,15u);
                a.StencilOpSeparate(g::FrontAndBack,g::Keep,increment ? g::Incr : g::Decr,g::Keep);
            }
            if (p.engineShadowPass!=EngineShadowPass::Fade && p.shadowBeaming) {
                // SetBeaming (Desktop 0xd5cb0) does NOT enable
                // blending. It changes the factors, color mask and current
                // RGBA for this and subsequent casters. Two-sided shadow-array
                // drawing has already overridden its stencil write mask.
                a.ColorMask(1,1,1,1);
                enabled(a,g::Blend,p.shadowBlendEnabled);
                a.BlendFuncSeparate(g::SrcAlpha,g::One,g::SrcAlpha,g::One);
                a.Uniform1i(u.overrideColor,1);
                a.Uniform4fv(u.color,1,p.shadowBeamColor.data());
            }
        }
        a.PolygonMode(g::FrontAndBack,p.wireframe ? g::Line : g::Fill);
        const auto primitive=p.primitive==PrimitiveTopology::QuadStrip ? g::QuadStrip :
            p.primitive==PrimitiveTopology::TriangleStrip ? g::TriangleStrip :
            p.primitive==PrimitiveTopology::TriangleFan ? g::TriangleFan : g::Triangles;
        if(desktopFixedFragment) {
            a.Disable(desktop_programs::VP);a.Disable(desktop_programs::FP);
            applyDesktopFixedTextureState(a,i.fixedApi,nextMaterialState);
            applyDesktopFixedVertexInputs(a,i.fixedProgram,nextMaterialState);
            a.Uniform1i(u.fixedDirtyUnit,dirty?static_cast<int>(dirtyPlan.volumeUnit):-1);
            a.Uniform1i(u.fixedUv1Array,engineProgram==MaterialProgram::Lightmap ||
                engineProgram==MaterialProgram::EnvironmentLightmap || (dirty&&dirtyPlan.lightmap));
            enabled(a,g::AlphaTest,!engineNoAlphaTest && material.alphaTest);
            i.fixedApi.AlphaFunc(0x0204,material.alphaCutoff);
            const float fixedFog[]={p.fog.color.x,p.fog.color.y,p.fog.color.z,1};
            i.fixedApi.Fogfv(0x0b66,fixedFog);i.fixedApi.Fogf(0x0b63,p.fog.start);
            i.fixedApi.Fogf(0x0b64,p.fog.end);i.fixedApi.Fogf(0x0b62,p.fog.density);
            i.fixedApi.Fogf(0x0b65,static_cast<float>(p.fog.mode==model::FogMode::Exponential?0x800:
                p.fog.mode==model::FogMode::ExponentialSquared?0x801:0x2601));
            enabled(a,g::Fog,p.fog.mode!=model::FogMode::Disabled);
        }
        if(desktopEngineBlur&&!p.outlineOnly) {
            // RenderSkinnedVPBlur's command34 is empty in BOTH Desktop dispatchers.
            // Execute its original VP arithmetic with the retained engine
            // fragment identity/enable, env registers and per-target textures.
            auto& programs=i.blurPrograms;
            if(!programs.ready) {
                if(!programs.x.load(programs.loader,error))return false;
                programs.ready=true;
            }
            auto& x=programs.x;
            desktop_programs::LegacyGuard engineRestore(a,x,&p);
            const auto vertex=programs.program(a,desktop_programs::VP,skin_programs::PosedMotionBlur,error);
            if(!vertex)return false;
            a.UseProgram(0);x.BindProgramARB(desktop_programs::VP,vertex);a.Enable(desktop_programs::VP);
            const std::array<float,4> direction{{p.skinBlurDirection.x,p.skinBlurDirection.y,p.skinBlurDirection.z,1}};
            const std::array<float,4> lateral{{p.skinBlurSideScale,0,0,0}};
            const std::array<float,4> speed{{p.skinBlurSpeed,p.skinMaximumBlurLength,.01f,.85f}};
            const std::array<float,4> literals{{0,2,3,1}},origin{{0,0,0,.15f}};
            const std::array<float,4> side{{p.skinBlurSideDirection.x,p.skinBlurSideDirection.y,p.skinBlurSideDirection.z,.5f}};
            x.ProgramLocalParameter4fvARB(desktop_programs::VP,12,direction.data());
            x.ProgramLocalParameter4fvARB(desktop_programs::VP,13,lateral.data());
            x.ProgramLocalParameter4fvARB(desktop_programs::VP,14,speed.data());
            x.ProgramLocalParameter4fvARB(desktop_programs::VP,16,literals.data());
            x.ProgramLocalParameter4fvARB(desktop_programs::VP,17,origin.data());
            x.ProgramLocalParameter4fvARB(desktop_programs::VP,69,side.data());
            applyDesktopFixedTextureState(a,x,nextMaterialState);
            if(nextMaterialState.fragmentProgramEnabled) {
                const auto fragment=programs.program(a,desktop_programs::FP,nextMaterialState.fragmentProgram,error);
                if(!fragment)return false;
                x.BindProgramARB(desktop_programs::FP,fragment);
                for(unsigned n=0;n<nextMaterialState.fragmentConstants.size();++n)
                    x.ProgramLocalParameter4fvARB(desktop_programs::FP,n,nextMaterialState.fragmentConstants[n].data());
                a.Enable(desktop_programs::FP);
            } else a.Disable(desktop_programs::FP);
            enabled(a,g::AlphaTest,material.alphaTest);x.AlphaFunc(0x0204,material.alphaCutoff);
            const float fogColor[]={p.fog.color.x,p.fog.color.y,p.fog.color.z,1};
            x.Fogfv(0x0b66,fogColor);x.Fogf(0x0b63,p.fog.start);x.Fogf(0x0b64,p.fog.end);
            x.Fogf(0x0b62,p.fog.density);x.Fogf(0x0b65,static_cast<float>(
                p.fog.mode==model::FogMode::Exponential?0x800:p.fog.mode==model::FogMode::ExponentialSquared?0x801:0x2601));
            enabled(a,g::Fog,p.fog.mode!=model::FogMode::Disabled);
            indexedDraw(primitive);enginePrimary=true;
        }
        if(!p.outlineOnly && !enginePrimary) indexedDraw(primitive);
        if((p.selected || p.outlineOnly) && !p.wireframe &&
           p.skinEffect==SkinRenderEffect::None) {
            if(desktopFixedFragment) {
                for(unsigned unit=0;unit<6;++unit) {
                    a.ActiveTexture(g::Texture0+unit);a.Disable(g::Texture2D);a.Disable(g::TextureCube);
                    a.Disable(0x806f);a.Disable(0x0de0);
                }
                a.Disable(g::Fog);a.Disable(g::AlphaTest);
            }
            const float selectedColor[]={1.0f,0.7f,0.1f,1.0f};
            a.Uniform1i(u.overrideColor,1); a.Uniform4fv(u.color,1,selectedColor);
            a.Disable(g::Blend); a.DepthMask(0); a.PolygonMode(g::FrontAndBack,g::Line); a.LineWidth(2.0f);
            indexedDraw(primitive);
        }
        if(p.engineCpuSkin && p.skinEffect==SkinRenderEffect::BumpedOutShell) {
            auto texture=p.engineCpuSkinRestoreTexture;
            if(p.rebindEngineTexture) {
                texture=p.rebindEngineTexture(texture,error);
                if(!texture)return false;
            }
            a.ActiveTexture(g::Texture0);a.BindTexture(g::Texture2D,texture);
            nextMaterialState.bind(0,false,texture);
        }
        const auto drawError=a.GetError();
        if(drawError!=g::NoError) {
            error="OpenGL indexed mesh submission failed ("+std::to_string(drawError)+")";
            return false;
        }
    } catch(const std::exception& exception) { error=exception.what(); return false; }
    // The ordinary dispatch-32/37 cleanup disables targets, but does not
    // unbind 2D/cube objects. Skin dispatch-0 updates its bindings in the pass.
    // Selection outlines are a tool overlay, not another engine material draw.
    if(!p.outlineOnly) {
        if(k1DesktopScene) {
            if(p.particlePass) {
                // RenderStandard/MotionBlurred/Linked restore depth writes,
                // lighting and ordinary factors, retaining alpha/blend enable.
                i.sceneState.alphaTest=p.material.alphaTest;
                i.sceneState.blend=p.material.blending;
                i.sceneState.depthWrite=true;i.sceneState.lighting=true;
                i.sceneState.source=g::SrcAlpha;i.sceneState.destination=g::OneMinusSrcAlpha;
            } else if(p.lightsaberPass) {
                // PartLightsaber::Draw restores these on both return paths;
                // neither its ordinary nor additive branch changes the cutoff
                // written by the preceding PartTriMesh::Mat.
                if(p.engineLightsaberMaterial)
                    i.sceneState.alphaReference=p.material.alphaCutoff;
                i.sceneState.source=g::SrcAlpha;i.sceneState.destination=g::OneMinusSrcAlpha;
                i.sceneState.lighting=true;i.sceneState.cullFaces=true;
                i.sceneState.depthWrite=true;i.sceneState.stencilWriteMask=0xff;
            } else if(p.material.k2PartTriMeshState &&
                      p.material.engineGame==material::EngineMaterialGame::Kotor1 &&
                      p.engineFlareProgram==EngineFlareProgram::None &&
                      p.engineShadowPass==EngineShadowPass::None) {
                i.sceneState.alphaTest=(k1Rigid||engineSkin)
                    ?nextMaterialState.alphaTestEnabled:p.material.alphaTest;
                i.sceneState.alphaReference=(k1Rigid||engineSkin)
                    ?nextMaterialState.alphaReference:p.material.alphaCutoff;
            }
        }
        if(!dirty) {
            if(ordinaryFragmentCleanup>=0) {
                nextMaterialState.finishFragmentCommand(static_cast<unsigned>(ordinaryFragmentCleanup));
            }
            if(desktopForce && !desktopFixedForce) {
                nextMaterialState.forceSightFragmentCommand=0;
                nextMaterialState.finishFragmentCommand(p.ownerFragment==OwnerFragmentEffect::Hologram?77u:74u);
            }
            if(desktopFixedForce && engineProgram==MaterialProgram::Lightmap)
                nextMaterialState.fixedCurrentStage=0; // client reset0xcc852
            if(desktopFixedForce && (engineProgram==MaterialProgram::Environment ||
               engineProgram==MaterialProgram::EnvironmentLightmap ||
               p.ownerFragment==OwnerFragmentEffect::IlluminationEnvironment)) {
                // RenderEMLM0xd05e5 also disables cube on unit1, even
                // though its dispatcher bound the environment on unit2.
                const unsigned unit=p.material.engineHardware.multitexture?1:nextMaterialState.fixedActiveTextureUnit;
                if(p.material.engineHardware.cubeMap || engineProgram==MaterialProgram::EnvironmentLightmap)
                    nextMaterialState.fixedTextureUnits[unit].enabled[1]=false;
                nextMaterialState.fixedCurrentStage=0;
                if(p.material.engineHardware.multitexture)nextMaterialState.fixedActiveTextureUnit=0;
            }
            if(p.skinEffect==SkinRenderEffect::MotionBlur &&
               (p.material.engineGame==material::EngineMaterialGame::Kotor1||p.material.engineHardware.arbFragmentPrograms)) {
                nextMaterialState.finishFragmentCommand(0,p.material.engineGame==material::EngineMaterialGame::Kotor1);
            }
            if(p.engineCpuSkin && p.skinEffect==SkinRenderEffect::None &&
               !p.material.skinEnvironmentAvailable && !p.material.engineSkinBumpTangent &&
               !p.material.engineHardware.nv1xLevelShaders && !p.material.engineHardware.nv2xLevelShaders &&
               p.material.engineHardware.multitexture && p.material.engineHardware.vertexProgramsNv)
                nextMaterialState.bind(1,false,0);
            if(p.engineCpuSkin && p.skinEffect==SkinRenderEffect::None &&
               !p.material.skinEnvironmentAvailable && p.material.engineSkinBumpTangent &&
               p.material.engineHardware.nv1xLevelShaders && !p.material.engineAtiValidation)
                for(unsigned unit=0;unit<4;++unit)nextMaterialState.bind(unit,false,0);
            impl_->materialState=nextMaterialState;
        }
        if(cpuBumpFallback && impl_->sceneActive)impl_->sceneState.alphaTest=true;
        retainDesktopStandardVertexState(mesh,p,engineProgram,impl_->desktopStandard);
    }
    return true;
}
void configureParticlePass(neomdl::model::EmitterBlend blend,unsigned framePass,MeshDrawParameters& p) {
    using Blend=neomdl::model::EmitterBlend;
    using Pass=neomdl::material::RenderPass;
    p.particlePass=true;p.streamedGeometry=true;p.lighting=false;p.lightCount=0;
    p.material.pass=blend==Blend::PunchThrough ? Pass::PunchThrough :
        (blend==Blend::Additive || framePass>0) ? Pass::Additive : Pass::Translucent;
    p.material.blending=p.material.pass!=Pass::PunchThrough;
    p.material.depthWrite=false;
    p.material.blendFunction=(blend==Blend::Additive || framePass>0)
        ? neomdl::material::BlendFunction::Additive
        : blend==Blend::PunchThrough
            ? neomdl::material::BlendFunction::Replace
            : neomdl::material::BlendFunction::Alpha;
    p.material.alphaTest=blend==Blend::PunchThrough;
    p.material.textureAlphaIsOpacity=true;
    if(blend==Blend::Additive || framePass>0) p.fog.mode=neomdl::model::FogMode::Disabled;
}
bool ShaderMeshRenderer::drawEngineScene(const EngineSceneSettings& settings,
        const EngineSceneStageCallback& callback,std::string& error) {
    error.clear();
    if(!ready() || !callback) {error="Engine scene requires a renderer and bucket callback";return false;}
    auto& i=*impl_;
    if(i.sceneActive) {error="Nested engine scene execution is not supported";return false;}
    if(model::engineSceneStageOrder(settings.game).empty()) {
        error="The selected game has no implemented engine scene sequence";return false;
    }
    if(settings.effects.enabled && !settings.worldEffectsEnabled) {
        error="Scene effects require the full world-effects scene pass";return false;
    }
    if(settings.game==model::GameVersion::Kotor2 && settings.distortionProducerCount && !settings.effects.enabled) {
        error="Distortion owners require the engine scene-effect graph";return false;
    }
    for(float value:settings.sceneAmbient)if(!std::isfinite(value)) {
        error="Non-finite engine scene ambient";return false;
    }
    try {
        StateGuard host(i.api);
        i.sceneActive=true;i.sceneGame=settings.game;
        auto& partMaterial=i.materialState.partMaterials[settings.game==model::GameVersion::Kotor1?0:1];
        partMaterial.materialHasChanged=true; // Scene::Render before ManageSceneBSP
        // The outer guard restores host state after the engine epilogue.
        // Failures retain no target binding or partial temporal-history commit.
        struct Finish {
            Impl& i;bool sceneFinished{};
            void scene() {
                if(sceneFinished)return;
                finishEngineScene(i.sceneState);
                applyEngineSceneRasterState(i.api,i.sceneState);sceneFinished=true;
            }
            ~Finish() {
                if(i.distortionActive) {
                    i.sceneState.colorWrite.fill(true);i.sceneState.stencilWriteMask=0xff;
                }
                i.sceneEffects.cancel();i.distortionActive=false;scene();i.sceneActive=false;
            }
        } finish{i};
        if(!i.sceneEffects.begin(i.api,settings.effects,settings.distortionProducerCount,
            settings.bindEffectTexture,i.materialState,error,settings.game,settings.sceneFog))return false;
        g::Int bits=0;i.api.GetIntegerv(g::StencilBits,&bits);
        const auto actions=[&](const EngineSceneStageActions& a) {
            applyEngineSceneRasterState(i.api,i.sceneState);
            if(a.clearStencil)i.api.Clear(g::StencilBufferBit);
            i.api.UseProgram(0);
            return true;
        };
        for(const auto stage:model::engineSceneStageOrder(settings.game)) {
            if(!actions(enterEngineSceneStage(stage,settings,bits,i.sceneState)))return false;
            if(stage==model::K2SceneStage::StaticMeshGeometry)
                i.desktopStandard.fixedEmission={{0,0,0,1}}; // RenderStaticGeometry material reset
            EngineSceneStageResult result;
            if(stage==model::K2SceneStage::Distortion) {
                if(i.sceneEffects.active()&&!i.sceneEffects.beginDistortion(error))return false;
                i.distortionActive=settings.distortionProducerCount!=0;
            }
            const bool ok=callback(stage,result,error);
            if(stage==model::K2SceneStage::Distortion) {
                i.sceneEffects.endDistortion();i.distortionActive=false;
            }
            if(!ok) {if(error.empty())error="Engine scene bucket failed";return false;}
            if(stage==model::K2SceneStage::StaticTriangleBins&&settings.grassEnabled)partMaterial.materialHasChanged=true;
            if(stage==model::K2SceneStage::FramebufferEffects && !i.sceneEffects.desktopEffects1(error))return false;
            if(stage==model::K2SceneStage::FramebufferEffects2 && !i.sceneEffects.desktopEffects2(error))return false;
            if(!actions(leaveEngineSceneStage(stage,settings,bits,result,i.sceneState)))return false;
        }
        if(settings.game==model::GameVersion::Kotor1&&!i.sceneEffects.finish(error))return false;
        finish.scene();
        if(settings.game==model::GameVersion::Kotor2&&!i.sceneEffects.finish(error))return false;
        return true;
    } catch(const std::exception& e) {error=e.what();return false;}
}
void ShaderMeshRenderer::invalidateSceneHistory() noexcept {impl_->sceneEffects.invalidateHistory();}
void ShaderMeshRenderer::beginK1DesktopEmitter(bool distortion) {
    // RenderFunc Desktop0x2f9e33, before selecting the render helper.
    auto& i=*impl_;auto& s=i.sceneState;
    s.stencilTest=distortion;s.colorWrite.fill(!distortion);
    s.stencilWriteMask=distortion?1u:0u;
    if(distortion) {
        s.stencilFunction=g::Always;s.stencilReference=1;s.stencilReadMask=0xff;
        s.stencilFail=s.stencilDepthFail=g::Keep;s.stencilDepthPass=g::Replace;
    }
    applyEngineSceneRasterState(i.api,s);
}
void ShaderMeshRenderer::finishK1DesktopEmitter() {
    // Distorted RenderFunc exit Desktop0x2fb254; a normal emitter already
    // established exactly these mask/enable values at entry.
    auto& i=*impl_;auto& s=i.sceneState;
    s.colorWrite.fill(true);s.stencilWriteMask=0;s.stencilTest=false;
    applyEngineSceneRasterState(i.api,s);
}

bool ShaderMeshRenderer::drawEngineGrass(const GrassScene& scene,const EngineGrassRenderSettings& settings,
        const GrassTextureBinding& bind,std::string& error) {
    error.clear();if(!ready()){error="Grass requires an initialized renderer";return false;}
    auto& i=*impl_;try {StateGuard host(i.api);
        const bool ok=i.grass.draw(i.api,scene,settings,bind,i.sceneState,
            i.materialState,i.desktopStandard,i.stats.drawCalls,error);
        return ok;
    }catch(const std::exception& e){error=e.what();return false;}
}
bool ShaderMeshRenderer::drawEngineLensFlares(const std::vector<LensFlareQuad>& quads,
        const model::Mat4& projection,const FlareTextureBinding& bindTexture,
        std::string& error,const EngineFlareRenderSettings& settings) {
    error.clear();
    if(!ready()) {error="Engine lens flares require an initialized renderer";return false;}
    if(!bindTexture || (settings.program!=EngineFlareProgram::DesktopArb &&
        settings.program!=EngineFlareProgram::FixedFunction) ||
        !std::isfinite(settings.desaturation)) {
        error="Invalid engine flare consumer inputs";return false;
    }
    for(float v:projection.values)if(!std::isfinite(v)) {error="Invalid flare projection";return false;}
    // Preflight geometry before any texture bind/controller event or output.
    try {model::Mesh check;for(const auto& quad:quads)buildLensFlareQuad(quad,check);}
    catch(const std::exception& e) {error=e.what();return false;}
    auto& i=*impl_;
    try {
        StateGuard guard(i.api);
        MeshDrawParameters p;p.projection=projection;p.lightCount=0;p.lighting=false;
        const bool legacyForce=settings.program==EngineFlareProgram::DesktopArb &&
            !settings.hardware.arbFragmentPrograms;
        p.material.engineHardware=settings.hardware;
        p.engineDesktopFixedFragment=legacyForce;
        if(legacyForce)p.fixedTextureMatrix=i.materialState.textureMatrix(0);
        p.engineFlareProgram=legacyForce?EngineFlareProgram::FixedFunction:settings.program;
        p.flareDesaturation=settings.desaturation;
        // The legacy dispatcher only enables texture 0 for command 56; it
        // does not execute the ARB desaturation shader. Both callers then
        // call the env[1] setter, including an empty list; its write is gated
        // by AuroraUseARBShaders (Desktop0xc4e66..0xc4ecf,0xf91bc).
        if(legacyForce && !applyDesktopFixedFragmentCommand(56,p,i.materialState,error))return false;
        if(settings.program==EngineFlareProgram::DesktopArb) {
            if(!legacyForce) {
                i.materialState.fragmentConstants[1]={{0,0,0,0.75f}};
                i.materialState.bindFragmentProgram(desktop_programs::k2FragmentProgram(56));
                i.materialState.fragmentConstants[7]={{.3f,.59f,.11f,settings.desaturation}};
            }
        }
        p.material.alphaTest=i.sceneActive?i.sceneState.alphaTest:settings.alphaTest;
        p.material.alphaCutoff=0;
        p.material.depthWrite=false;p.cullBackfaces=settings.cullBackfaces;
        p.streamedGeometry=true;
        for(const auto& quad:quads) {
            p.diffuseTexture=bindTexture(quad,error);
            if(!p.diffuseTexture) {if(error.empty())error="Engine flare texture is missing";return false;}
            if(legacyForce) {
                // CAurTexture::Bind changes target state after the outer 56.
                auto& unit=i.materialState.fixedTextureUnits[0];
                if(settings.hardware.cubeMap)unit.enabled[1]=false;
                unit.enabled[2]=false;
                i.materialState.bind(0,false,p.diffuseTexture);
            }
            buildLensFlareQuad(quad,i.flareStream);
            if(!draw(i.flareStream,nullptr,++i.flareRevision,p,error))return false;
        }
        // EnableFragmentShaders(0x4a): clear 2D bindings 1..3, retain cube
        // targets. Keep env[1].a=.75 and unit0's last bound flare texture.
        if(legacyForce) {
            if(!applyDesktopFixedFragmentCommand(74,p,i.materialState,error))return false;
        } else if(settings.program==EngineFlareProgram::DesktopArb) {
            i.materialState.finishFragmentCommand(74);
        }
        return true;
    } catch(const std::exception& e) {error=e.what();return false;}
}

bool ShaderMeshRenderer::drawEngineShadows(
        const std::vector<EngineShadowVolumeDraw>& volumes,const neomdl::model::Mesh& fadeFan,
        const neomdl::model::Mat4& view,const neomdl::model::Mat4& projection,std::string& error,
        EngineShadowStencilMode mode,const EngineShadowEffects& effects) {
    error.clear();
    if (!ready()) { error="Shadow renderer has no current initialized context";return false; }
    if (mode!=EngineShadowStencilMode::TwoPassDepthFail && mode!=EngineShadowStencilMode::TwoSidedDepthPass) {
        error="Unknown engine shadow stencil mode";return false;
    }
    if (effects.softMode!=EngineSoftShadowMode::Disabled &&
        effects.softMode!=EngineSoftShadowMode::DesktopARB &&
        effects.softMode!=EngineSoftShadowMode::DesktopATI) {
        error="Unsupported engine soft-shadow branch";return false;
    }
    if (!std::all_of(effects.beamColor.begin(),effects.beamColor.end(),
                     [](float v){return std::isfinite(v);}) ||
        !std::isfinite(effects.softDarkening)) {
        error="Non-finite engine shadow effect state";return false;
    }
    const auto valid=[](const neomdl::model::Mesh& geometry) {
        for (const auto& vertex:geometry.vertices)
            if (!std::isfinite(vertex.position.x)||!std::isfinite(vertex.position.y)||!std::isfinite(vertex.position.z)) return false;
        for (const auto index:geometry.indices) if (index>=geometry.vertices.size()) return false;
        return geometry.indices.size()<=static_cast<std::size_t>(std::numeric_limits<g::Size>::max());
    };
    const auto validMatrix=[](const neomdl::model::Mat4& matrix) {
        return std::all_of(matrix.values.begin(),matrix.values.end(),
                           [](float value){return std::isfinite(value);});
    };
    if (!validMatrix(view) || !validMatrix(projection)) {
        error="Invalid engine shadow camera transform";return false;
    }
    if (!valid(fadeFan)) {error="Invalid engine shadow fade geometry";return false;}
    for (const auto& volume:volumes) if (!valid(volume.geometry) || !validMatrix(volume.modelView)) {
        error="Invalid engine shadow volume geometry or transform";return false;
    }
    if (fadeFan.indices.empty()) return true;
    auto& i=*impl_;auto& a=i.api;
    g::Int bits=0;a.GetIntegerv(g::StencilBits,&bits);
    if (bits<=0 || bits>8) { error="Engine shadow path requires a 1..8-bit stencil attachment";return false; }
    StateGuard restore(a);
    // Allocation/capability checks and the scene-color copy precede any clear.
    // A failed later draw restores saved color before the outer state guard.
    struct CancelSoft { SoftShadowPass& soft; ~CancelSoft(){soft.cancel();} } cancel{i.softShadows};
    const bool soft=effects.softMode!=EngineSoftShadowMode::Disabled;
    if (soft && !i.softShadows.begin(a,effects.softDarkening,effects.softMode,i.materialState,effects.fog,error)) return false;
    const auto threshold=1u<<static_cast<unsigned>(bits-1);
    a.StencilMaskSeparate(g::FrontAndBack,0xffu);
    a.ClearStencil(static_cast<g::Int>(threshold-1u));a.Clear(g::StencilBufferBit);
    MeshDrawParameters p;
    p.lighting=false;p.lightCount=0;p.cullBackfaces=true;p.streamedGeometry=true;
    p.projection=projection;p.material.depthWrite=false;p.material.alphaTest=false;
    p.primitive=PrimitiveTopology::TriangleStrip;
    p.shadowBlendEnabled=restore.blend!=0;
    p.shadowBeamColor=effects.beamColor;
    for (const auto& volume:volumes) {
        if (volume.beginsBeaming) {
            p.shadowBeaming=true;
            p.shadowStencilOff=effects.beamingStencilOff;
        }
        if (volume.geometry.indices.empty()) continue;
        // Engine two-sided RenderShadowArray sets all write bits itself. The
        // override persists until another caller-side SetBeaming transition.
        if (mode==EngineShadowStencilMode::TwoSidedDepthPass) p.shadowStencilOff=false;
        i.shadowStream=volume.geometry;p.modelView=volume.modelView;
        const auto revision=++i.shadowRevision;
        if (mode==EngineShadowStencilMode::TwoSidedDepthPass) {
            p.engineShadowPass=EngineShadowPass::TwoSidedDepthPass;
            if (!draw(i.shadowStream,nullptr,revision,p,error)) return false;
        } else {
            p.engineShadowPass=EngineShadowPass::Increment;
            if (!draw(i.shadowStream,nullptr,revision,p,error)) return false;
            p.engineShadowPass=EngineShadowPass::Decrement;
            if (!draw(i.shadowStream,nullptr,revision,p,error)) return false;
        }
    }
    i.shadowStream=fadeFan;p.modelView=view;p.primitive=PrimitiveTopology::TriangleFan;
    p.engineShadowPass=EngineShadowPass::Fade;p.shadowStencilThreshold=threshold;
    p.shadowSoftMask=soft;p.shadowBeaming=false;
    if (!draw(i.shadowStream,nullptr,++i.shadowRevision,p,error)) return false;
    if (!soft) return true;
    if (!i.softShadows.finish(error)) return false;
    if (effects.softMode==EngineSoftShadowMode::DesktopATI) {
        // Non-ARB FrameBufferModificationPixelShadersATI(2) and finish clear
        // these engine bindings independently of the host GL state restore.
        for(unsigned unit=0;unit<4;++unit)i.materialState.bind(unit,false,0);
        i.materialState.bind(3,true,0);
    }
    return true;
}

bool ShaderMeshRenderer::drawShadowVolumeInspection(
        const std::vector<EngineShadowVolumeDraw>& volumes,
        const neomdl::model::Mat4& projection,std::string& error) {
    error.clear();
    if (!ready()) {error="Shadow inspector has no current initialized context";return false;}
    auto& i=*impl_;
    StateGuard restore(i.api);
    i.api.Disable(g::StencilTest);
    MeshDrawParameters p;
    p.lighting=false;p.lightCount=0;p.cullBackfaces=false;p.streamedGeometry=true;
    p.projection=projection;p.material.depthWrite=false;p.wireframe=true;
    p.primitive=PrimitiveTopology::TriangleStrip;
    // Explicit inspector color, not a engine material/effect substitute.
    p.hasColorOverride=true;p.colorOverride={0.2f,0.8f,1.0f,1.0f};
    for (const auto& volume:volumes) {
        if (volume.geometry.indices.empty()) continue;
        i.shadowStream=volume.geometry;p.modelView=volume.modelView;
        if (!draw(i.shadowStream,nullptr,++i.shadowRevision,p,error)) return false;
    }
    return true;
}

void ShaderMeshRenderer::setGobNewSorting(bool enabled) noexcept {
    impl_->gobNewSorting=enabled;
}
void ShaderMeshRenderer::setForceSight(const material::ForceSightSettings& value) noexcept {
    impl_->forceSight=value;
}
bool ShaderMeshRenderer::gobNewSorting() const noexcept {return impl_->gobNewSorting;}
void resolveSceneSubmission(MeshSubmission& submission) {
    requireSceneOwnerLifetime(submission.ownerLifetime);
    if(submission.resolveOwner)submission.resolveOwner(submission);
}
bool ShaderMeshRenderer::drawQueue(const std::vector<MeshSubmission>& input,std::string& error,
                                   QueueOrdering ordering, const SubmissionPreparation& prepare,
                                   const OwnerDrawCompletion& completedOwner,
                                   const OwnerDrawBegin& beginOwner,
                                   const OwnerDrawBegin& animateOwner) {
    if(!ready()) {error="Mesh shader backend is not initialized";return false;}
    const bool gobBucket=ordering==QueueOrdering::K2DynamicGobs||ordering==QueueOrdering::K1DynamicGobs||
        ordering==QueueOrdering::K2HologramGobs;
    std::vector<MeshSubmission> submissions=input;
    const auto expired=[](const MeshSubmission& s) {
        return s.ownerLifetime&&s.ownerLifetime->expired();
    };
    // Gob destruction removes ordinary/hologram bucket entries separately
    // from SetScene. Late emitter/saber/blur buckets retain borrowed pointers.
    if(gobBucket)submissions.erase(std::remove_if(submissions.begin(),submissions.end(),expired),submissions.end());
    try {
        for(auto& s:submissions)resolveSceneSubmission(s);
    } catch(const std::exception& e) {error=e.what();return false;}
    std::vector<neomdl::material::QueueKey> keys;
    keys.reserve(submissions.size());
    for(const auto& submission:submissions) {
        if(!submission.mesh && !(submission.ownerOnly && submission.sceneOwner && !submission.drawPrimary && !submission.trailingShell && ordering!=QueueOrdering::K2StaticMeshes && !submission.parameters.selected)) {
            error="Null mesh in scene render queue";return false;
        }
        // Keep a particle emitter's base and four neighboring-frame passes
        // adjacent. Their blend states differ but their sorting key does not.
        keys.push_back({submission.parameters.particlePass
                ? neomdl::material::RenderPass::Translucent
                : submission.parameters.material.pass,
            submission.eyeDepth, submission.authoredOrderGroup,
            submission.authoredOrder});
    }
    std::vector<std::size_t> order;
    std::vector<std::vector<std::size_t>> gobGroups;
    std::vector<std::size_t> liveGobOrder;
    if(ordering==QueueOrdering::Submission) {
        order.resize(submissions.size());
        std::iota(order.begin(),order.end(),0u);
    } else if(ordering==QueueOrdering::K2StaticMeshes) {
        std::vector<neomdl::model::StaticMeshBucketEntry> entries;
        entries.reserve(submissions.size());
        for(std::size_t index=0;index<submissions.size();++index) {
            const auto& submission=submissions[index];
            const bool textured=submission.parameters.diffuseTextureOrder!=0u ||
                submission.parameters.diffuseTexture!=0u;
            const auto identity=textured
                ? (submission.parameters.diffuseTextureOrder!=0u
                    ? submission.parameters.diffuseTextureOrder
                    : static_cast<std::uint64_t>(submission.parameters.diffuseTexture))
                : 0u;
            entries.push_back({index,identity,identity,
                submission.mesh->backgroundGeometry});
        }
        order=neomdl::model::staticMeshBucketOrder(entries);
    } else if(gobBucket) {
        struct GobGroup {
            neomdl::model::GobBucketEntry key;
            std::vector<std::size_t> submissions;
        };
        std::vector<GobGroup> groups;
        std::unordered_map<std::uint64_t,std::size_t> groupByOwner;
        groups.reserve(submissions.size());
        for(std::size_t index=0;index<submissions.size();++index) {
            const auto& submission=submissions[index];
            std::size_t groupIndex=groups.size();
            if(submission.sceneOwner!=0u) {
                const auto found=groupByOwner.find(submission.sceneOwner);
                if(found!=groupByOwner.end()) groupIndex=found->second;
            }
            if(groupIndex==groups.size()) {
                GobGroup group;
                group.key.sourceIndex=groupIndex;
                group.key.ownerIdentity=submission.sceneOwner!=0u
                    ? submission.sceneOwner
                    : std::numeric_limits<std::uint64_t>::max()-index;
                group.key.insertionOrder=groupIndex;
                group.key.alpha=submission.ownerAlpha;
                group.key.modelClassification=submission.ownerModelClassification;
                group.key.hasTransparency=submission.ownerHasTransparency;
                group.key.cameraDistance=submission.ownerCameraDistance;
                group.submissions.push_back(index);
                groups.push_back(std::move(group));
                if(submission.sceneOwner!=0u)
                    groupByOwner.emplace(submission.sceneOwner,groupIndex);
            } else {
                auto& group=groups[groupIndex];
                group.key.hasTransparency=group.key.hasTransparency ||
                    submission.ownerHasTransparency;
                group.submissions.push_back(index);
            }
        }
        std::vector<neomdl::model::GobBucketEntry> entries;
        entries.reserve(groups.size());
        for(const auto& group:groups) entries.push_back(group.key);
        std::vector<std::size_t> groupOrder;
        const auto game=ordering==QueueOrdering::K1DynamicGobs
            ? model::GameVersion::Kotor1 : model::GameVersion::Kotor2;
        try {groupOrder=neomdl::model::gobBucketDrawOrder(game,entries,impl_->gobNewSorting,impl_->forceSight.enabled,false);}
        catch(const std::exception& e) {error=e.what();return false;}
        liveGobOrder.assign(groupOrder.rbegin(),groupOrder.rend());
        for(auto& group:groups)gobGroups.push_back(std::move(group.submissions));
    } else {
        order=neomdl::material::renderQueueOrder(keys);
    }
    std::vector<bool> admitted(submissions.size(),false);
    const auto drawGroup=[&](const std::vector<std::size_t>& indices) {
        auto& first=submissions[indices.front()];
        try {
            // Keep the sort's pre-animation keys. Gob::Animate has now run;
            // each Part borrows its current deformation/material/transform.
            if(beginOwner||animateOwner)for(const auto index:indices)resolveSceneSubmission(submissions[index]);
            // VisibilityCheck's enablevisibilitytest global is zero and has
            // no internal producer in the supplied builds: its active return
            // is true. Coarse BSP admission already formed this Gob bucket.
        } catch(const std::exception& e) {error=e.what();return false;}
        if(beginOwner&&first.sceneOwner&&!beginOwner(first,error))return false;
        for(const auto index:indices) {
            const auto& s=submissions[index];admitted[index]=true;
            auto parameters=s.parameters;parameters.selected=false;
            if (prepare && !prepare(s,parameters,error)) return false;
            if(s.drawPrimary||s.materialOnly) {
                if(s.prepareTextures && !s.prepareTextures(parameters,error)) return false;
                if(s.materialOnly) {if(!consumeEnginePartMaterial(*s.mesh,parameters,error))return false;}
                else if(!draw(*s.mesh,s.deformed,s.poseRevision,parameters,error)) return false;
            }
            if(s.trailingShell) {
                auto shell=*s.trailingShell;
                applyEngineDoorAlpha(shell,parameters.engineDoorAlpha);
                if(shell.engineCpuSkin && parameters.fixedFunctionLights) {
                    // Desktop CPU shells call RenderFlat even when the primary uses
                    // environment VP lighting. Reuse the fixed-family snapshot
                    // from that same owner selection without advancing its fade.
                    const auto& fixed=*parameters.fixedFunctionLights;
                    shell.lightCount=std::min(fixed.size(),shell.lights.size());
                    std::copy_n(fixed.begin(),shell.lightCount,shell.lights.begin());
                    shell.sceneAmbient=parameters.sceneAmbient;
                }
                if(s.prepareTextures && !s.prepareTextures(shell,error)) return false;
                if(!draw(*s.mesh,s.deformed,s.poseRevision,shell,error)) return false;
            }
        }
        return !completedOwner||!first.sceneOwner||completedOwner(submissions,indices,error);
    };
    if(gobBucket) {
        const auto removeDestroyed=[&] {
            liveGobOrder.erase(std::remove_if(liveGobOrder.begin(),liveGobOrder.end(),[&](auto group) {
                return expired(submissions[gobGroups[group].front()]);
            }),liveGobOrder.end());
        };
        // Holograms use the same Force Sight-aware comparator, but their
        // caller has one reverse traversal (Desktop 0xc2ce3).
        // Only DoGobBuckets partitions character/door color passes.
        const bool forceSight=ordering==QueueOrdering::K2DynamicGobs&&impl_->forceSight.enabled;
        for(unsigned pass=0;pass<(forceSight?2u:1u);++pass) {
            removeDestroyed();
            for(auto count=liveGobOrder.size();count!=0;--count) {
                removeDestroyed();
                if(count>liveGobOrder.size())continue;
                const auto group=liveGobOrder[count-1u];
                const auto& indices=gobGroups[group];
                const auto& first=submissions[indices.front()];
                if(forceSight&&bool(first.ownerModelClassification&0x0cu)!=(pass!=0))continue;
                if(animateOwner&&first.sceneOwner&&!animateOwner(first,error))return false;
                removeDestroyed();
                // DoGobBuckets retains the fetched Gob pointer, but rechecks
                // the live shifted list length after Animate (K2 Desktop c28fd).
                // Lower-slot removal may make this same Gob recur next time.
                if(count>liveGobOrder.size())continue;
                if(!drawGroup(indices))return false;
            }
        }
    } else {
        for(std::size_t ordinal=0;ordinal<order.size();) {
            const auto& first=submissions[order[ordinal]];
            auto end=ordinal+1u;
            if(first.sceneOwner)while(end<order.size()&&submissions[order[end]].sceneOwner==first.sceneOwner)++end;
            std::vector<std::size_t> indices(order.begin()+static_cast<std::ptrdiff_t>(ordinal),
                order.begin()+static_cast<std::ptrdiff_t>(end));
            if(animateOwner&&first.sceneOwner&&!animateOwner(first,error))return false;
            if(!drawGroup(indices))return false;
            ordinal=end;
        }
    }
    for(std::size_t index=0;index<submissions.size();++index) {
        const auto& s=submissions[index];
        if(!admitted[index]||impl_->distortionActive||!s.parameters.selected||s.parameters.wireframe)continue;
        auto parameters=s.parameters; parameters.outlineOnly=true;
        if(!draw(*s.mesh,s.deformed,s.poseRevision,parameters,error)) return false;
    }
    return true;
}
void ShaderMeshRenderer::retireGeometry(const model::Mesh* geometry) {
    const auto found=impl_->meshes.find(geometry);
    if(found==impl_->meshes.end())return;
    const auto& mesh=found->second;
    for(const auto buffer:{mesh.vbo,mesh.ebo,mesh.skinVbo,mesh.rigidVbo})
        if(buffer)impl_->retiredBuffers.push_back(buffer);
    impl_->meshes.erase(found);
}
void ShaderMeshRenderer::flushRetiredGeometry() {
    for(const auto buffer:impl_->retiredBuffers)impl_->api.DeleteBuffers(1,&buffer);
    impl_->retiredBuffers.clear();
}
void ShaderMeshRenderer::clearGeometry() {
    if(!ready()) return;
    while(!impl_->meshes.empty())retireGeometry(impl_->meshes.begin()->first);
    flushRetiredGeometry();
}
EngineMaterialState& ShaderMeshRenderer::materialState() noexcept { return impl_->materialState; }
const EngineMaterialState& ShaderMeshRenderer::materialState() const noexcept { return impl_->materialState; }
void ShaderMeshRenderer::forgetTexture(g::UInt texture) noexcept {
    impl_->materialState.forgetTexture(texture);
}
bool ShaderMeshRenderer::drawDialogueFade(std::uint32_t rgba,std::string& error) {
    error.clear();
    if(!ready()){error="Dialogue fade requires an initialized renderer";return false;}
    auto& i=*impl_;auto& a=i.api;
    try {
        StateGuard host(a);
        if(!i.dialogueFadeProgram) {
            // Translation of the untextured, unlit immediate GUI quad. Its
            // constant fragment is the engine glColor4ubv conversion, with no
            // mesh material, postprocessing or invented alpha modulation.
            const char* vertex="#version 120\nattribute vec2 aPosition;void main(){gl_Position=vec4(aPosition,0.0,1.0);}";
            const char* fragment="#version 120\nuniform vec4 uColor;void main(){gl_FragColor=uColor;}";
            const auto vs=compile(a,g::VertexShader,vertex,error);if(!vs)return false;
            const auto fs=compile(a,g::FragmentShader,fragment,error);
            if(!fs){a.DeleteShader(vs);return false;}
            const auto program=a.CreateProgram();a.AttachShader(program,vs);a.AttachShader(program,fs);
            a.BindAttribLocation(program,0,"aPosition");a.LinkProgram(program);
            a.DeleteShader(vs);a.DeleteShader(fs);
            g::Int linked{};a.GetProgramiv(program,g::LinkStatus,&linked);
            if(!linked){error="Dialogue fade shader link failed: "+shaderLog(a,program,true);a.DeleteProgram(program);return false;}
            i.dialogueFadeProgram=program;i.dialogueFadeColor=a.GetUniformLocation(program,"uColor");
        }
        if(!i.dialogueFadeVbo) {
            constexpr float vertices[]={-1,-1,1,-1,1,1,-1,1};
            constexpr std::uint32_t indices[]={0,1,2,3};
            a.GenBuffers(1,&i.dialogueFadeVbo);a.GenBuffers(1,&i.dialogueFadeIbo);
            a.BindBuffer(g::ArrayBuffer,i.dialogueFadeVbo);a.BufferData(g::ArrayBuffer,sizeof(vertices),vertices,g::StaticDraw);
            a.BindBuffer(g::ElementArrayBuffer,i.dialogueFadeIbo);a.BufferData(g::ElementArrayBuffer,sizeof(indices),indices,g::StaticDraw);
        }
        // GUI retains engine culling, masks, blend equations and fixed alpha
        // testing. The surrounding host GL state is not the engine state.
        applyEngineSceneRasterState(a,i.sceneState);
        a.Disable(g::Lighting);a.Disable(g::DepthTest);a.Disable(g::Texture2D);
        a.Enable(g::Blend);a.BlendFuncSeparate(g::SrcAlpha,g::OneMinusSrcAlpha,g::SrcAlpha,g::OneMinusSrcAlpha);
        std::array<float,4> color{};
        for(unsigned n=0;n<4;++n)color[n]=static_cast<float>((rgba>>(8*n))&0xffu)/255.0f;
        a.UseProgram(i.dialogueFadeProgram);a.Uniform4fv(i.dialogueFadeColor,1,color.data());
        a.BindBuffer(g::ArrayBuffer,i.dialogueFadeVbo);
        a.EnableVertexAttribArray(0);a.VertexAttribPointer(0,2,g::Float,0,2*sizeof(float),nullptr);
        a.BindBuffer(g::ElementArrayBuffer,i.dialogueFadeIbo);
        a.DrawElements(g::TriangleFan,4,g::UnsignedInt,nullptr);++i.stats.drawCalls;
        // SetupViewport reenables lighting/texture; StopLayer reenables depth.
        i.sceneState.lighting=true;i.sceneState.texture2D=true;i.sceneState.depthTest=true;
        i.sceneState.blend=true;i.sceneState.source=g::SrcAlpha;i.sceneState.destination=g::OneMinusSrcAlpha;
        const auto status=a.GetError();
        if(status!=g::NoError){error="OpenGL error in dialogue fade: "+std::to_string(status);return false;}
        return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
void ShaderMeshRenderer::shutdown() {
    if(impl_->normalizationCube)impl_->api.DeleteTextures(1,&impl_->normalizationCube);
    impl_->grass.shutdown(impl_->api);impl_->desktopStandard={};
    impl_->normalizationCube=0;impl_->materialState.reset();impl_->skinMaterials.shutdown();impl_->blurPrograms.shutdown();impl_->k1Materials.shutdown();
    if(impl_->dialogueFadeProgram)impl_->api.DeleteProgram(impl_->dialogueFadeProgram);
    if(impl_->dialogueFadeVbo)impl_->api.DeleteBuffers(1,&impl_->dialogueFadeVbo);
    if(impl_->dialogueFadeIbo)impl_->api.DeleteBuffers(1,&impl_->dialogueFadeIbo);
    impl_->dialogueFadeProgram=impl_->dialogueFadeVbo=impl_->dialogueFadeIbo=0;
    impl_->sceneState=EngineSceneRasterState{};
    if(!ready()) return;
    clearGeometry();
    impl_->sceneEffects.shutdown();
    impl_->softShadows.shutdown();
    impl_->api.DeleteProgram(impl_->program); impl_->program=0;
    if(impl_->fixedProgram)impl_->api.DeleteProgram(impl_->fixedProgram);
    impl_->fixedProgram=0;
}
} // namespace neomdl::render
