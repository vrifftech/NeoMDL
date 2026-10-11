#include "render/K1MaterialPass.hpp"
#include "core/SceneQueries.hpp"
#include "render/DesktopMaterialPrograms.hpp"
#include "render/K1MaterialPrograms.hpp"
#include "render/SkinMaterialPrograms.hpp"
#include "render/ShaderMeshRenderer.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cmath>
#include <neoshared/glu/ByteMipmaps.hpp>

namespace neomdl::render {
namespace {
namespace g=gl;
using namespace desktop_programs;
using Path=material::TriMeshRenderPath;
Path selectedPath(const MeshDrawParameters& p) {
    if(!p.engineCpuSkin)return p.material.renderPath;
    if(p.material.skinEnvironmentAvailable)return Path::EnvironmentMapped;
    return p.material.engineSkinBumpTangent&&(!p.material.engineHardware.nv1xLevelShaders||p.material.engineAtiValidation)?Path::BumpmapFallback:Path::Flat;
}
#define K1_FIXED_GL(X) \
 X(g::Boolean,IsTexture,(g::UInt)) \
 X(void,DeleteTextures,(g::Size,const g::UInt*)) \
 X(void,PushClientAttrib,(g::UInt)) X(void,PopClientAttrib,()) \
 X(void,EnableClientState,(g::Enum)) X(void,DisableClientState,(g::Enum)) \
 X(void,ClientActiveTexture,(g::Enum)) \
 X(void,VertexPointer,(g::Int,g::Enum,g::Size,const void*)) \
 X(void,NormalPointer,(g::Enum,g::Size,const void*)) \
 X(void,ColorPointer,(g::Int,g::Enum,g::Size,const void*)) \
 X(void,TexCoordPointer,(g::Int,g::Enum,g::Size,const void*)) \
 X(void,TexGeni,(g::Enum,g::Enum,g::Int)) \
 X(void,Lightfv,(g::Enum,g::Enum,const float*)) \
 X(void,Lightf,(g::Enum,g::Enum,float)) \
 X(void,LoadIdentity,()) X(void,MultMatrixf,(const float*)) \
 X(void,Color4fv,(const float*)) X(void,MultiTexCoord4fv,(g::Enum,const float*))
struct FixedApi {
#define DECLARE(ret,name,args) using name##Fn=ret (NEOMDL_GL_CALL*)args;name##Fn name{};
 K1_FIXED_GL(DECLARE)
#undef DECLARE
 bool load(const g::Loader& loader,std::string& error) {
#define LOAD(ret,name,args) name=reinterpret_cast<name##Fn>(loader("gl" #name));if(!name){error="Engine K1 material requires gl" #name;return false;}
 K1_FIXED_GL(LOAD)
#undef LOAD
 return true;
 }
};
#undef K1_FIXED_GL
void enabled(g::Api& a,g::Enum cap,bool value){if(value)a.Enable(cap);else a.Disable(cap);}
// GLRender::InitializeATIFragmentShaders 0x2e2294 and 0x2e334d. Unlike
// K2's ordinary VP path, fixed texgen and matrices produce these coordinates.
const char* lightmapFragment=R"ARB(!!ARBfp1.0
TEMP base, lm, primary;
TEX base, fragment.texcoord[0], texture[0], 2D;
TEX lm, fragment.texcoord[1], texture[1], 2D;
ADD primary.rgb, fragment.color.primary, lm;
MUL base.a, fragment.color.primary.a, base.a;
MUL base.rgb, base, primary;
MOV result.color, base;
END
)ARB";
std::string environmentFragment(bool cube,bool lightmap) {
    if(!lightmap) {
        // K1's non-fog environment FP has the same LRP as K2's fog variant.
        std::string text=cube?skin_programs::CubeEnvironmentFogFragment:skin_programs::EnvironmentFogFragment;
        const std::string fog="LRP r0.rgb, v1.r, c[1], r0;\n";
        text.erase(text.find(fog),fog.size());return text;
    }
    return std::string("!!ARBfp1.0\nTEMP base,lm,env,rgb,term;\n")+
        "TEX base, fragment.texcoord[0], texture[0], 2D;\n"
        "TEX lm, fragment.texcoord[1], texture[1], 2D;\n"
        "TEX env, fragment.texcoord[2], texture[2], "+(cube?"CUBE":"2D")+";\n"
        "MUL rgb.rgb, fragment.color.primary, base;\n"
        "MAD rgb.rgb, lm, base, rgb;\n"
        "SUB term.a, 1.0, base.a;\n"
        "MUL term.rgb, env, term.a;\n"
        "ADD rgb.rgb, rgb, term;\n"
        "MOV rgb.a, program.local[0].a;\nMOV result.color, rgb;\nEND\n";
}
std::string fixedFogFragment(std::string source,model::FogMode fog) {
    // ATI text fragments retain the normal post-fragment fog stage.
    // The ARB translation requests that same stage explicitly.
    if(fog!=model::FogMode::Disabled)
        source.insert(source.find('\n')+1,fog==model::FogMode::Exponential?"OPTION ARB_fog_exp;\n":fog==model::FogMode::ExponentialSquared?"OPTION ARB_fog_exp2;\n":"OPTION ARB_fog_linear;\n");
    return source;
}
std::string nv1xFragment(const MeshDrawParameters& p,bool environment) {
    // EnableCombiners command26 0x30d189, command21 0x30ca16.
    // General-combiner sum outputs saturate before the next stage.
    std::string text="!!ARBfp1.0\n";
    text+="TEMP base,other,value;\nTEX base,fragment.texcoord[0],texture[0],2D;\n";
    text+="TEX other,fragment.texcoord[1],texture[1],";text+=environment&&p.environmentCube?"CUBE;\n":"2D;\n";
    if(environment)text+="SUB value.a,1.0,base.a;\nMUL_SAT value.rgb,other,value.a;\nMOV value.a,1.0;\n";
    else text+="ADD_SAT value.rgb,fragment.color.primary,other;\nMUL_SAT value.rgb,base,value;\nMUL value.a,fragment.color.primary.a,base.a;\n";
    text+="MOV result.color,value;\nEND\n";return fixedFogFragment(text,p.fog.mode);
}
const std::string& litStandard(ProgramCache& programs,const MeshDrawParameters& p,const char* body=k1_programs::Standard) {
    return programs.lightProgram(body,std::vector<PreviewLight>(p.lights.begin(),p.lights.begin()+std::min(p.lightCount,p.lights.size())));
}
}
std::vector<EngineRigidVertex> engineRigidVertices(const model::Mesh& mesh,const model::DeformedMesh* pose) {
    std::vector<EngineRigidVertex> result;result.reserve(mesh.vertices.size());
    for(std::size_t n=0;n<mesh.vertices.size();++n) {
        const auto& source=mesh.vertices[n];EngineRigidVertex v;
        v.position=pose&&pose->valid?pose->positions[n]:source.position;
        v.normal=pose&&pose->valid?(pose->rawSkinNormals.empty()?model::deformedNormalAt(mesh,*pose,n):pose->rawSkinNormals[n]):mesh.normalAt(n);
        for(unsigned c=0;c<4;++c)v.color[c]=source.color[c]/255.0f;
        v.uv0=source.texcoord0;v.uv1=source.texcoord1;
        v.basis4=source.tangentBasis.bitangent;v.basis5=source.tangentBasis.tangent;v.basis6=source.tangentBasis.normal;
        result.push_back(v);
    }
    return result;
}
struct K1MaterialPass::Impl : ProgramCache {
    FixedApi fixed; g::UInt lookup{};
    void makeLookup(g::Api& a,EngineMaterialState& state) {
        // K1 MakeLookUpTexture 0x30bb7a, invoked by command29 before its
        // six Bind calls. Preserve allocation/binding lifetime even though
        // this ATI material never samples the lookup texture.
        if(fixed.IsTexture(lookup))return;
        neoshared::glu::ByteImage source{256,256,2,{}};source.bytes.resize(256*256*2);
        for(unsigned y=0;y<256;++y){const auto alpha=static_cast<std::uint8_t>(std::pow(static_cast<double>(y)/255.0,5.0)*255.99);for(unsigned x=0;x<256;++x){float lum=static_cast<float>(x)/255.0f;lum=lum*255.99000549316406f;source.bytes[(y*256+x)*2]=static_cast<std::uint8_t>(lum);source.bytes[(y*256+x)*2+1]=alpha;}}
        a.GenTextures(1,&lookup);a.ActiveTexture(g::Texture0);a.BindTexture(g::Texture2D,lookup);state.bind(0,false,lookup);
        g::Int maximum{};a.GetIntegerv(0x0d33,&maximum);
        const auto levels=neoshared::glu::buildByteMipmaps(source,static_cast<unsigned>(maximum));
        for(std::size_t n=0;n<levels.size();++n){const auto& level=levels[n];a.TexImage2D(g::Texture2D,static_cast<g::Int>(n),0x190a,level.width,level.height,0,0x190a,0x1401,level.bytes.data());}
        a.TexParameteri(g::Texture2D,0x2802,0x812f);a.TexParameteri(g::Texture2D,0x2803,0x812f);a.TexParameteri(g::Texture2D,0x2800,0x2601);a.TexParameteri(g::Texture2D,0x2801,0x2703);
    }
};
K1MaterialPass::K1MaterialPass():impl_(std::make_unique<Impl>()){}
K1MaterialPass::~K1MaterialPass()=default;
void K1MaterialPass::setLoader(const g::Loader& loader){impl_->loader=loader;}
bool K1MaterialPass::prepare(g::Api& a,const MeshDrawParameters& p,std::string& error) {
    auto& i=*impl_;
    if(!i.ready){if(!i.loader||!i.x.load(i.loader,error)||!i.fixed.load(i.loader,error))return false;
    i.ready=true;}
    switch(selectedPath(p)) {
    case Path::LightMapped: return i.program(a,FP,fixedFogFragment(lightmapFragment,p.fog.mode),error)!=0;
    case Path::EnvironmentMapped: return i.program(a,FP,fixedFogFragment(environmentFragment(p.environmentCube,false),p.fog.mode),error)!=0;
    case Path::EMLM:return i.program(a,FP,fixedFogFragment(environmentFragment(p.environmentCube,true),p.fog.mode),error)!=0;
    case Path::EMBM:
        return i.program(a,VP,litStandard(i,p),error)&&i.program(a,VP,std::string(k1_programs::Shiny)+"END\n",error)&&i.program(a,FP,skin_programs::ShinyFragment,error);
    case Path::LightMappedBumpSpecular:
    case Path::LightMappedBumpDiffuse:
        if(p.skinBumpLights.empty())return i.program(a,FP,fixedFogFragment(lightmapFragment,p.fog.mode),error)!=0;
        // Engine 0x347xxx feeds these NV strings to glProgramStringARB;
        // neither its constructor nor its ATI command8..12 translates them.
        return i.program(a,VP,std::string(p.material.renderPath==Path::LightMappedBumpSpecular?k1_programs::NvLightmapSpecular:k1_programs::NvLightmapDiffuse)+"END\n",error)!=0;
    case Path::BumpMapped:
        return p.skinBumpLights.empty()||(i.program(a,VP,litStandard(i,p,k1_programs::Bump),error)&&i.program(a,FP,fixedFogFragment(skin_programs::BumpFragment,p.fog.mode),error));
    default:return true;
    }
}
bool K1MaterialPass::draw(g::Api& a,const model::Mesh& mesh,const MeshDrawParameters& p,
    EngineMaterialState& state,g::UInt normalization,g::UInt vbo,g::Size,const std::vector<model::EngineMeshDraw>& batches,std::uint64_t& calls,std::string& error) {
    auto& i=*impl_;auto& x=i.x;auto& f=i.fixed;LegacyGuard restore(a,x,p.projection.values.data(),p.modelView.values.data());
    f.PushClientAttrib(0x2);
    struct ClientState { FixedApi& f; ~ClientState(){f.PopClientAttrib();} } client{f};
    a.UseProgram(0);a.Disable(VP);a.Disable(FP);a.Disable(g::ColorMaterial);
    a.BindBuffer(g::ArrayBuffer,vbo);
    for(unsigned slot=0;slot<15;++slot)a.DisableVertexAttribArray(slot);
    f.EnableClientState(0x8074);f.VertexPointer(3,g::Float,sizeof(EngineRigidVertex),reinterpret_cast<const void*>(offsetof(EngineRigidVertex,position)));
    f.EnableClientState(0x8075);f.NormalPointer(g::Float,sizeof(EngineRigidVertex),reinterpret_cast<const void*>(offsetof(EngineRigidVertex,normal)));
    if(p.enginePrelightPool&&p.enginePrelightPool->active&&!p.engineOnlyGob&&p.lighting&&!p.engineCpuSkin){f.EnableClientState(0x8076);f.ColorPointer(4,g::Float,sizeof(EngineRigidVertex),reinterpret_cast<const void*>(offsetof(EngineRigidVertex,color)));}
    else f.DisableClientState(0x8076);
    for(unsigned unit=0;unit<6;++unit) {
        a.ActiveTexture(g::Texture0+unit);a.Disable(g::Texture2D);a.Disable(g::TextureCube);
        for(unsigned k=0;k<3;++k)a.Disable(0x0c60+k);
        f.ClientActiveTexture(g::Texture0+unit);f.DisableClientState(0x8078);
    }
    const auto uv=[&](unsigned unit,bool lightmap){f.ClientActiveTexture(g::Texture0+unit);f.EnableClientState(0x8078);f.TexCoordPointer(2,g::Float,sizeof(EngineRigidVertex),reinterpret_cast<const void*>(lightmap?offsetof(EngineRigidVertex,uv1):offsetof(EngineRigidVertex,uv0)));};
    if(mesh.mdxTexcoordOffsets[0]!=0xffffffffu)uv(0,false);
    f.MultiTexCoord4fv(g::Texture0,p.engineFlatCurrentTexcoord.data());f.ClientActiveTexture(g::Texture0);
    // Engine retained matrices are independent of the host context stack.
    for(unsigned unit=0;unit<4;++unit){a.ActiveTexture(g::Texture0+unit);x.MatrixMode(0x1702);x.PushMatrix();const auto matrix=unit==0&&p.textureAngleDegrees!=0?engineTextureRotation(p.textureAngleDegrees):state.textureMatrix(unit);x.LoadMatrixf(matrix.values.data());}
    struct TextureMatrices {g::Api& a;LegacyApi& x;~TextureMatrices(){for(unsigned unit=0;unit<4;++unit){a.ActiveTexture(g::Texture0+unit);x.MatrixMode(0x1702);x.PopMatrix();}a.ActiveTexture(g::Texture0);}} matrices{a,x};
    a.ActiveTexture(g::Texture0);x.MatrixMode(ModelView);
    state.setNormalizeNormals(p.fixedFunctionNormalizeNormals.value_or(state.normalizeNormals()));enabled(a,0x0ba1,state.normalizeNormals());
    enabled(a,g::Lighting,p.lighting);
    const std::array<float,4> white{{1,1,1,1}},zero{{0,0,0,1}};
    f.Color4fv(p.partHasOwner||!p.lighting?white.data():zero.data());
    const auto& ambient=p.enginePartMaterial->ambient;
    const auto& diffuse=p.enginePartMaterial->diffuse;
    const auto& emission=p.enginePartMaterial->emission;
    const float global[]={p.partHasOwner?p.sceneAmbient[0]:0,p.partHasOwner?p.sceneAmbient[1]:0,p.partHasOwner?p.sceneAmbient[2]:0,1};
    a.Materialfv(g::FrontAndBack,g::MaterialAmbient,ambient.data());a.Materialfv(g::FrontAndBack,g::MaterialDiffuse,diffuse.data());a.Materialfv(g::FrontAndBack,g::MaterialEmission,emission.data());a.LightModelfv(g::LightModelAmbient,global);
    x.PushMatrix();f.LoadIdentity();
    for(unsigned n=0;n<8;++n){enabled(a,0x4000+n,n<p.lightCount);if(n>=p.lightCount)continue;const auto& light=p.lights[n];f.Lightfv(0x4000+n,0x1203,light.position.data());const float da[]={light.diffuse[0],light.diffuse[1],light.diffuse[2],1},am[]={light.ambient[0],light.ambient[1],light.ambient[2],1};f.Lightfv(0x4000+n,0x1201,da);f.Lightfv(0x4000+n,0x1200,am);f.Lightfv(0x4000+n,0x1202,zero.data());for(unsigned c=0;c<3;++c)f.Lightf(0x4000+n,0x1207+c,light.attenuation[c]);}
    x.PopMatrix();
    a.Enable(g::DepthTest);a.DepthFunc(g::Lequal);a.DepthMask(p.material.depthWrite);enabled(a,g::CullFace,p.cullBackfaces);a.CullFace(g::Back);a.FrontFace(model::transformHandedness(p.modelView)<0?g::CW:g::CCW);a.PolygonMode(g::FrontAndBack,p.wireframe?g::Line:g::Fill);
    enabled(a,g::Blend,p.material.blending);a.BlendEquationSeparate(g::FuncAdd,g::FuncAdd);
    const auto blend=[&](g::Enum s,g::Enum d){a.BlendFuncSeparate(s,d,s,d);};
    blend(p.material.blendFunction==material::BlendFunction::Replace?g::One:g::SrcAlpha,p.material.blendFunction==material::BlendFunction::Replace?g::Zero:p.material.blendFunction==material::BlendFunction::Additive?g::One:g::OneMinusSrcAlpha);
    enabled(a,g::AlphaTest,p.material.alphaTest);x.AlphaFunc(0x0204,p.material.alphaCutoff);
    const float fog[]={p.fog.color.x,p.fog.color.y,p.fog.color.z,1};x.Fogfv(0x0b66,fog);x.Fogf(0x0b63,p.fog.start);x.Fogf(0x0b64,p.fog.end);x.Fogf(0x0b62,p.fog.density);x.Fogf(0x0b65,static_cast<float>(p.fog.mode==model::FogMode::Exponential?0x800:p.fog.mode==model::FogMode::ExponentialSquared?0x801:0x2601));enabled(a,Fog,p.fog.mode!=model::FogMode::Disabled);
    const auto bind=[&](unsigned unit,bool cube,g::UInt texture,bool repeat){if(repeat&&texture&&p.rebindEngineTexture){texture=p.rebindEngineTexture(texture,error);if(!texture)return false;}state.bind(unit,cube,texture);auto& target=state.fixedTextureUnits[unit];
        if(p.material.engineHardware.cubeMap)target.enabled[1]=cube;
        target.enabled[2]=false;target.enabled[cube?1:0]=texture!=0;
        a.ActiveTexture(g::Texture0+unit);a.BindTexture(cube?g::TextureCube:g::Texture2D,texture);enabled(a,cube?g::TextureCube:g::Texture2D,texture!=0);a.ActiveTexture(g::Texture0);return true;};
    const auto fp=[&](std::string_view program){const auto id=i.program(a,FP,program,error);if(!id)return false;x.BindProgramARB(FP,id);i.setLocalParameters(FP,id,0,state.fragmentConstants.data(),state.fragmentConstants.size());a.Enable(FP);if(selectedPath(p)!=Path::NV1xEMLM||p.material.engineAtiValidation)state.bindFragmentProgram(program);return true;};
    const auto draw=[&](){
        static constexpr g::Enum modes[]={0,1,3,4,5,6,7,8,9,2};
        for(const auto& batch:batches){a.DrawElements(modes[batch.primitive],static_cast<g::Size>(batch.indexCount),g::UnsignedInt,reinterpret_cast<const void*>(batch.firstIndex*sizeof(std::uint32_t)));++calls;}
    };
    const auto texgen=[&](unsigned unit,bool reflection=false){a.ActiveTexture(g::Texture0+unit);for(unsigned k=0;k<(p.environmentCube||reflection?3u:2u);++k){f.TexGeni(0x2000+k,0x2500,p.environmentCube||reflection?0x8512:0x2402);a.Enable(0x0c60+k);}if(p.environmentCube){auto rotation=p.worldFromEye;rotation.values[12]=rotation.values[13]=rotation.values[14]=0;x.MatrixMode(0x1702);f.MultMatrixf(rotation.values.data());x.MatrixMode(ModelView);}a.ActiveTexture(g::Texture0);};
    // PartTriMesh::Mat's first Bind precedes the helper's command-specific binds.
    if(!bind(0,false,p.diffuseTexture,false))return false;
    x.TexEnvi(0x2300,0x2200,0x2100);
    const auto path=selectedPath(p);
    if(path==Path::LightMapped||path==Path::EnvironmentMapped||path==Path::EMLM||
       ((path==Path::LightMappedBumpSpecular||path==Path::LightMappedBumpDiffuse)&&p.skinBumpLights.empty()))
        state.fixedSecondTexture=true;
    if(path==Path::EnvironmentMapped||path==Path::EMLM)state.fixedEnvironmentMap=true;
    if(path==Path::Flat||path==Path::BumpmapFallback){if(path==Path::BumpmapFallback){a.Disable(g::AlphaTest);blend(g::One,g::Zero);}draw();}
    else if(path==Path::LightMapped){blend(g::SrcAlpha,g::OneMinusSrcAlpha);uv(1,true);if(!bind(0,false,p.diffuseTexture,true)||!bind(1,p.lightmapCube,p.lightmapTexture,true)||!fp(fixedFogFragment(lightmapFragment,p.fog.mode)))return false;draw();}
    else if(path==Path::EnvironmentMapped||path==Path::EMLM){const bool lm=path==Path::EMLM;a.Disable(g::AlphaTest);if(lm)uv(1,true);if(!bind(0,false,p.diffuseTexture,true)||!bind(lm?2:1,p.environmentCube,p.environmentTexture,true))return false;if(lm&&!bind(1,p.lightmapCube,p.lightmapTexture,true))return false;texgen(lm?2:1);state.fragmentConstants[0]={{1,1,1,p.material.environmentOpacity}};if(!fp(fixedFogFragment(environmentFragment(p.environmentCube,lm),p.fog.mode)))return false;draw();}
    else if(path==Path::NV1xEMLM){
        // Engine NV1x caller excludes owned parts, then issues26/32/21/32.
        if(p.material.engineHardware.nv1xLevelShaders&&!p.partHasOwner){
            if(!p.diffuseTexture||!p.lightmapTexture||!p.environmentTexture)draw();
            else {
                a.Disable(g::AlphaTest);a.Disable(VP);blend(g::One,g::Zero);uv(1,true);
                if(!bind(0,false,p.diffuseTexture,true)||!bind(1,p.lightmapCube,p.lightmapTexture,true)||!fp(p.material.engineAtiValidation?fixedFogFragment(lightmapFragment,p.fog.mode):nv1xFragment(p,false)))return false;
                draw();if(p.material.engineAtiValidation)state.disableFragmentProgram();a.Disable(FP);a.ActiveTexture(g::Texture1);a.Disable(g::Texture2D);a.Disable(g::TextureCube);a.ActiveTexture(g::Texture0);f.ClientActiveTexture(g::Texture1);f.DisableClientState(0x8078);f.ClientActiveTexture(g::Texture0);blend(g::One,g::One);
                if(!p.material.engineAtiValidation){
                    if(!bind(0,false,p.diffuseTexture,true)||!bind(1,p.environmentCube,p.environmentTexture,true))return false;
                    texgen(1);x.Fogfv(0x0b66,zero.data());state.fragmentConstants[0]={{1,1,1,1}};
                    if(!fp(nv1xFragment(p,true)))return false;
                }
                // In the ATI dispatcher command21 is a literal no-op, so
                // the second RenderFlat retains fixed unit0 texturing.
                draw();if(p.material.engineAtiValidation)state.disableFragmentProgram();a.Disable(FP);a.ActiveTexture(g::Texture1);a.Disable(g::Texture2D);a.Disable(g::TextureCube);for(unsigned k=0;k<3;++k)a.Disable(0x0c60+k);a.ActiveTexture(g::Texture0);x.Fogfv(0x0b66,fog);a.Enable(g::AlphaTest);blend(g::SrcAlpha,g::OneMinusSrcAlpha);
            }
        }
    }
    else if(path==Path::LightMappedNoShaders){
        // RenderLightMappedNoShaders0x3460ac: the engine missing-resource
        // branch is RenderFlat; otherwise keep its fixed multipass blending.
        if(!p.diffuseTexture||!p.lightmapTexture)draw();
        else {
            f.Color4fv(white.data());a.Disable(g::Lighting);
            if(p.material.engineHardware.multitexture&&!p.material.engineMaterialEnvironmentAvailable){
                blend(g::SrcAlpha,g::OneMinusSrcAlpha);
                if(!bind(0,false,p.lightmapTexture,true))return false;
                x.TexEnvi(0x2300,0x2200,0x1e01);
                if(!bind(1,false,p.diffuseTexture,true))return false;
                a.ActiveTexture(g::Texture1);x.TexEnvi(0x2300,0x2200,0x2100);a.ActiveTexture(g::Texture0);uv(0,true);uv(1,false);draw();
                uv(0,false);uv(1,true);f.ClientActiveTexture(g::Texture1);f.DisableClientState(0x8078);f.ClientActiveTexture(g::Texture0);a.ActiveTexture(g::Texture1);a.Disable(g::Texture2D);x.TexEnvi(0x2300,0x2200,0x2100);a.ActiveTexture(g::Texture0);x.TexEnvi(0x2300,0x2200,0x2100);if(!bind(0,false,p.diffuseTexture,true))return false;
            } else {
                if(!bind(0,false,p.diffuseTexture,true))return false;
                blend(g::One,g::Zero);draw();
                if(!bind(0,false,p.lightmapTexture,true))return false;
                blend(0x0306,g::Zero);uv(0,true);draw();uv(0,false);
                if(p.material.engineMaterialEnvironmentAvailable&&p.material.engineHardware.cubeMap){
                    a.Disable(g::AlphaTest);if(!bind(0,false,p.diffuseTexture,true))return false;blend(g::Zero,g::One);draw();
                    if(!bind(0,p.environmentCube,p.environmentTexture,true))return false;
                    blend(0x0304,g::One);texgen(0,true);draw();for(unsigned k=0;k<3;++k)a.Disable(0x0c60+k);a.Enable(g::AlphaTest);
                }
            }
            f.Color4fv(zero.data());a.Enable(g::Lighting);blend(g::SrcAlpha,g::OneMinusSrcAlpha);
        }
    }
    else {
        std::array<std::array<float,4>,96> constants{};
        constants[15]={{1,1,1,0}};constants[16]={{0,2,3,1}};constants[89]={{0,0,0,1}};
        for(unsigned row=0;row<3;++row)constants[92-row]={{p.worldFromEye.values[row],p.worldFromEye.values[4+row],p.worldFromEye.values[8+row],p.worldFromEye.values[12+row]}};
        for(unsigned row=0;row<3;++row)constants[93+row]=p.enginePartMaterial->vertex[row];
        for(unsigned n=0;n<std::min(p.lightCount,p.lights.size());++n){const auto& light=p.lights[n];auto& pos=constants[70+4*n];for(unsigned row=0;row<3;++row)pos[row]=p.worldFromEye.values[row]*light.position[0]+p.worldFromEye.values[4+row]*light.position[1]+p.worldFromEye.values[8+row]*light.position[2]+p.worldFromEye.values[12+row]*light.position[3];pos[3]=light.position[3];constants[71+4*n]={{light.attenuation[0],light.attenuation[1],light.attenuation[2],0}};constants[72+4*n]={{light.ambient[0],light.ambient[1],light.ambient[2],1}};constants[73+4*n]={{light.diffuse[0],light.diffuse[1],light.diffuse[2],1}};}
        const auto vp=[&](std::string_view program){const auto id=i.program(a,VP,program,error);if(!id)return false;x.BindProgramARB(VP,id);i.setLocalParameters(VP,id,0,constants.data(),constants.size());a.Enable(VP);return true;};
        const g::UInt slots[]={0,2,8,9,12,13,14};const g::Int sizes[]={3,3,2,2,3,3,3};const std::size_t offsets[]={offsetof(EngineRigidVertex,position),offsetof(EngineRigidVertex,normal),offsetof(EngineRigidVertex,uv0),offsetof(EngineRigidVertex,uv1),offsetof(EngineRigidVertex,basis4),offsetof(EngineRigidVertex,basis5),offsetof(EngineRigidVertex,basis6)};
        const auto attributes=[&](){for(unsigned n=0;n<7;++n){a.EnableVertexAttribArray(slots[n]);a.VertexAttribPointer(slots[n],sizes[n],g::Float,0,sizeof(EngineRigidVertex),reinterpret_cast<const void*>(offsets[n]));}const float current[]={0,0,0,1};a.VertexAttrib4fv(10,current);a.VertexAttrib4fv(11,current);};
        const auto cleanup=[&](){state.finishFragmentCommand(0,true);a.Disable(FP);x.BindProgramARB(FP,0);for(unsigned unit=0;unit<4;++unit){state.bind(unit,false,0);a.ActiveTexture(g::Texture0+unit);a.BindTexture(g::Texture2D,0);}a.ActiveTexture(g::Texture0);};
        if(path==Path::EMBM){
            attributes();a.Enable(g::Blend);a.Disable(g::AlphaTest);a.Disable(Fog);blend(g::One,g::Zero);
            if(p.material.engineWaterAlpha==1.0f){
                if(!vp(litStandard(i,p)))return false;
                if(p.material.useLightmap&&p.lightmapTexture){if(!bind(0,false,p.diffuseTexture,true)||!bind(1,p.lightmapCube,p.lightmapTexture,true)||!fp(lightmapFragment))return false;}
                else {cleanup();if(!bind(0,false,p.diffuseTexture,true))return false;}
                draw();state.disableFragmentProgram();a.Disable(FP);
            }
            // Command29's lookup allocation precedes bump/normalization/env.
            i.makeLookup(a,state);
            if(!bind(0,false,p.bumpTexture,true))return false;
            for(unsigned unit=1;unit<=4;++unit)if(!bind(unit,true,normalization,false))return false;
            if(!bind(5,true,p.environmentTexture,true))return false;
            constants[15]={{1,0,1,0}};
            if(p.material.engineWaterAlpha<1.0f){blend(g::SrcAlpha,g::OneMinusSrcAlpha);state.fragmentConstants[0]={{1,1,1,p.material.engineWaterAlpha}};}
            else blend(0x0305,g::One);
            if(!vp(std::string(k1_programs::Shiny)+"END\n")||!fp(skin_programs::ShinyFragment))return false;
            draw();cleanup();
            if(p.fog.mode!=model::FogMode::Disabled){enabled(a,Fog,true);blend(g::SrcAlpha,g::OneMinusSrcAlpha);if(!vp(std::string(k1_programs::StandardFog)+"END\n")||!fp(skin_programs::FogFragment))return false;
            draw();}
            cleanup();
        }else if(path==Path::LightMappedBumpSpecular||path==Path::LightMappedBumpDiffuse){
            if(p.skinBumpLights.empty()){blend(g::SrcAlpha,g::OneMinusSrcAlpha);uv(1,true);if(!bind(0,false,p.diffuseTexture,true)||!bind(1,p.lightmapCube,p.lightmapTexture,true)||!fp(fixedFogFragment(lightmapFragment,p.fog.mode)))return false;
            draw();}
            else if(!vp(std::string(path==Path::LightMappedBumpSpecular?k1_programs::NvLightmapSpecular:k1_programs::NvLightmapDiffuse)+"END\n"))return false;
        }else if(path==Path::BumpMapped){
            if(p.skinBumpLights.empty())draw();
            else {
                attributes();a.Disable(g::AlphaTest);
                if(!bind(0,false,p.bumpTexture,true)||!bind(1,true,normalization,false)||!bind(2,true,normalization,false)||!bind(3,false,p.diffuseTexture,true))return false;
                auto base=p;base.lightCount=0;for(std::size_t n=0;n<std::min(p.lightCount,p.lights.size());++n)if(std::none_of(p.skinBumpLights.begin(),p.skinBumpLights.end(),[&](const auto& light){return light.id==p.lights[n].id;}))base.lights[base.lightCount++]=p.lights[n];
                for(unsigned n=0;n<base.lightCount;++n){const auto& light=base.lights[n];auto& pos=constants[70+4*n];for(unsigned row=0;row<3;++row)pos[row]=p.worldFromEye.values[row]*light.position[0]+p.worldFromEye.values[4+row]*light.position[1]+p.worldFromEye.values[8+row]*light.position[2]+p.worldFromEye.values[12+row]*light.position[3];pos[3]=light.position[3];constants[71+4*n]={{light.attenuation[0],light.attenuation[1],light.attenuation[2],0}};constants[72+4*n]={{light.ambient[0],light.ambient[1],light.ambient[2],1}};constants[73+4*n]={{light.diffuse[0],light.diffuse[1],light.diffuse[2],1}};}
                const auto local=[&](model::Vec3 q){return engineRotate(engineInverse(p.enginePartOrientation),{q.x-p.enginePartPosition.x,q.y-p.enginePartPosition.y,q.z-p.enginePartPosition.z});};
                const auto camera=local({p.worldFromEye.values[12],p.worldFromEye.values[13],p.worldFromEye.values[14]});
                for(const auto& light:p.skinBumpLights){const auto position=local(light.position);constants[12]={{position.x,position.y,position.z,1}};constants[13]={{camera.x,camera.y,camera.z,1}};constants[14]={{1,0,9.0f/(2.0f*light.radius*light.radius),1}};state.bumpVertexConstants={{constants[12],constants[13],constants[14]}};for(unsigned c=0;c<3;++c){state.fragmentConstants[0][c]=light.color[c]*p.material.engineSpecularColor[c]*p.material.engineSpecularBumpIntensity;state.fragmentConstants[1][c]=light.color[c]*diffuse[c]*p.material.engineDiffuseBumpIntensity;}state.fragmentConstants[0][3]=state.fragmentConstants[1][3]=1;if(!vp(litStandard(i,base,k1_programs::Bump))||!fp(fixedFogFragment(skin_programs::BumpFragment,p.fog.mode)))return false;draw();enabled(a,g::AlphaTest,p.material.alphaTest);}
                // Command13 disables the program and auxiliary targets;
                // unlike command0 it does not unbind the retained textures.
                state.finishFragmentCommand(13,true);a.Disable(FP);
                for(unsigned unit=1;unit<=3;++unit){a.ActiveTexture(g::Texture0+unit);a.Disable(g::Texture2D);a.Disable(g::TextureCube);}
                a.ActiveTexture(g::Texture0);a.Enable(g::Lighting);a.Disable(VP);
            }
        }
    }
    // These helpers finish with dispatcher32/37, retaining the bound program.
    if(path==Path::LightMapped||path==Path::EnvironmentMapped||path==Path::EMLM||
       ((path==Path::LightMappedBumpSpecular||path==Path::LightMappedBumpDiffuse)&&p.skinBumpLights.empty()))
        state.finishFragmentCommand(path==Path::EMLM?37u:32u,true);
    state.alphaTestEnabled=a.IsEnabled(g::AlphaTest)!=0;
    a.GetFloatv(g::AlphaTestRef,&state.alphaReference);
    const auto code=a.GetError();if(code!=g::NoError){error="Engine K1 material submission failed ("+std::to_string(code)+")";return false;}return true;
}
void K1MaterialPass::shutdown(){auto& i=*impl_;if(i.lookup){i.fixed.DeleteTextures(1,&i.lookup);i.lookup=0;}i.shutdown();}
} // namespace neomdl::render
